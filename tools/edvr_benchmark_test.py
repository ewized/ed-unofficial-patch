"""Offline fixtures for the EDVR capture qualification tool."""

from contextlib import redirect_stderr, redirect_stdout
from copy import deepcopy
import hashlib
from io import StringIO
import json
from pathlib import Path
import tempfile
import unittest

from edvr_benchmark import main, qualify_capture


def digest(data):
    return hashlib.sha256(data).hexdigest()


def packet(sequence, time_ms, native, generated, repeated, session_key='a'):
    return dict(kind='presentation', now_ms=time_ms, captured_at_ms=time_ms, fresh=True,
                sequence=sequence, session_key=session_key, native=native,
                generated=generated, repeated=repeated)


def window(start=1000, invalid=0, missing=0):
    return dict(kind='benchmark', start_ms=start, end_ms=start+30000, sample_ms=30000,
                finished_ms=start+32000, drain_ms=2000, status='completed',
                scope=1, cpu={'p50_ms': 1}, gpu={'p50_ms': 2},
                cpu_valid=20, gpu_valid=20, cpu_invalid=0, cpu_missing=0,
                gpu_invalid=invalid, gpu_missing=missing, aa='off', model='k', build='a'*8,
                input_dimensions=[2000]*4, output_dimensions=[2000]*4,
                eligible=not (invalid or missing))


def capture(records=None):
    records = deepcopy(records if records is not None else [window()])
    gfx = [item for item in records if not item['kind'].startswith('runtime_')]
    runtime = [item for item in records if item['kind'].startswith('runtime_')]
    sources = {}
    for role, items in (('gfx', gfx), ('openxr', runtime)):
        lines = [f'{role} fixture line {line}' for line in range(1, len(items)+1)]
        data = ('\n'.join(lines) + '\n').encode()
        sources[role] = data
        for line, (item, text) in enumerate(zip(items, lines), 1):
            item.update(source_sha256=digest(data), line_number=line,
                        source_line_sha256=digest(text.encode()))
    target = dict(pid=60, creation_ticks='134000000000000001', session=1, image='EliteDangerous64.exe')
    run = dict(expected_build='a'*40, build_verified=True, aa='native', vrs=False,
               fg='auto', config_sha256='c'*64, warmup_end_ms=1000, end_ms=100000, target_windows=1)
    row = dict(nominal_mode='native', vrs='off', frame_generation='auto', supported=True,
               expected_output_dimensions=[2000]*4, ini_sha256='c'*64)
    receipt = dict(build=run['expected_build'], config_sha256=run['config_sha256'],
                   warmup_end_ms=run['warmup_end_ms'], end_ms=run['end_ms'], session_id='private-owner',
                   target_before=target, target_after=target.copy(),
                   sources={role: dict(sha256=digest(data), bytes=len(data)) for role, data in sources.items()})
    qualification = dict(requested=dict(aa='native', vrs='off', fg='auto'),
                         physical_scanout_qualified=False,
                         treatments=dict(aa=dict(status='matched'), vrs=dict(status='off'),
                                         fg=dict(status='expected_inactive')))
    return dict(run=run, row=row, records=records, qualification=qualification, receipt=receipt,
                collection_status='complete'), sources


class BenchmarkQualificationTests(unittest.TestCase):
    def test_counter_specific_coverage_and_total_common_span(self):
        args, sources = capture([window(),
                                 packet(1, 2000, 10, 0, 5), packet(5, 3000, 13, 0, 6),
                                 packet(14, 4000, 20, None, 8), packet(22, 5000, 27, 0, 9)])
        result = qualify_capture(**args, sources=sources)
        submission = result['submission']
        self.assertEqual(submission['rates_per_second'], dict(native=17/3, generated=0, repeated=4/3, total=4))
        self.assertEqual(submission['known_counter_coverage']['generated'],
                         dict(seconds=1, intervals=1, fraction_of_observed=1/3, fraction_of_row=1/99))
        self.assertEqual(result['features']['fg']['status'], 'partial')
        self.assertIsNone(result['features']['fg']['generated_observed'])
        self.assertEqual(result['timing']['status'], 'complete')

    def test_unknown_and_zero_are_distinct(self):
        args, sources = capture([window(), packet(1, 2000, 1, None, 0), packet(4, 3000, 4, None, 0)])
        result = qualify_capture(**args, sources=sources)
        self.assertEqual(result['submission']['rates_per_second'],
                         dict(native=3, generated=None, repeated=0, total=None))
        self.assertIsNone(result['features']['fg']['generated_observed'])
        self.assertEqual(result['features']['fg']['status'], 'unavailable')

    def test_selected_multiplier_does_not_prove_generation(self):
        args, sources = capture([
            packet(1, 2000, 1, 0, 0) | {'selected_multiplier': 2},
            packet(7, 3000, 4, 0, 3) | {'selected_multiplier': 2},
        ])
        result = qualify_capture(**args, sources=sources)
        self.assertEqual(result['features']['fg']['status'], 'expected_inactive')
        self.assertIs(result['features']['fg']['generated_observed'], False)
        self.assertEqual(result['submission']['rates_per_second']['total'], 6)
        args['qualification']['treatments']['fg']['status'] = 'active'
        result = qualify_capture(**args, sources=sources)
        self.assertEqual(result['features']['fg']['status'], 'inconsistent')

    def test_reused_session_or_reordered_packets_cannot_inflate_coverage(self):
        args, sources = capture([packet(1, 2000, 1, 0, 0), packet(11, 4000, 11, 0, 0),
                                 packet(1, 3000, 90, 30, 20, 'b') | {'now_ms': 4100},
                                 packet(30, 5000, 100, 40, 29, 'b'),
                                 packet(36, 6000, 106, 40, 29, 'b')])
        result = qualify_capture(**args, sources=sources)
        self.assertEqual(result['submission']['counts']['native'], 16)
        self.assertEqual(result['submission']['observed_seconds'], 3)

    def test_same_build_reused_pid_birth_or_session_change_refused(self):
        args, sources = capture()
        for key, changed in (('creation_ticks', '134000000000000002'), ('session', 2)):
            with self.subTest(key=key):
                modified = deepcopy(args)
                modified['receipt']['target_after'][key] = changed
                with self.assertRaisesRegex(ValueError, 'Process identity changed'):
                    qualify_capture(**modified, sources=sources)
        args['run']['expected_build'] = args['receipt']['build'] = 'a'*8
        with self.assertRaisesRegex(ValueError, 'Exact verified build'):
            qualify_capture(**args, sources=sources)

    def test_source_replacement_and_mixed_record_refused(self):
        args, sources = capture()
        with self.assertRaisesRegex(ValueError, 'hash or size differs'):
            qualify_capture(**args, sources=sources | {'gfx': sources['gfx'] + b'other'})
        args['records'][0]['line_number'] += 1
        with self.assertRaisesRegex(ValueError, 'Record line differs'):
            qualify_capture(**args, sources=sources)

    def test_native_equal_but_wrong_and_runtime_unknown_input_remain_distinct(self):
        args, sources = capture([window(), dict(kind='runtime_cycle', input_dimensions=None,
                                                input_status='unavailable', raw_input_dimensions=[0]*4,
                                                eligible=True, aggregate_admitted=True)])
        result = qualify_capture(**args, sources=sources)
        self.assertEqual(result['features']['aa']['status'], 'matched')
        self.assertEqual(result['runtime_input_metadata'], dict(known_windows=0, unavailable_windows=1))
        args['row']['expected_output_dimensions'] = [2200]*4
        result = qualify_capture(**args, sources=sources)
        self.assertEqual(result['features']['aa']['status'], 'wrong_mode')
        self.assertEqual(result['timing']['status'], 'complete')
        self.assertFalse(result['physical_scanout_qualified'])

    def test_dlss_one_wrong_eye_cannot_pass_agreement(self):
        measured = window() | {'aa': 'dlss', 'input_dimensions': [1100]*4,
                               'output_dimensions': [2000, 2000, 1900, 2000]}
        args, sources = capture([measured])
        args['run']['aa'] = args['row']['nominal_mode'] = args['qualification']['requested']['aa'] = 'balanced'
        args['row']['expected_input_dimensions'] = [1100]*4
        result = qualify_capture(**args, sources=sources)
        self.assertEqual(result['features']['aa']['status'], 'wrong_mode')

    def test_history_and_unsplittable_drain_do_not_count_as_row_faults(self):
        args, sources = capture([window(1000, invalid=20), window(40000, missing=7),
                                 window(71000), window(90500, invalid=3)])
        for context in (args['run'], args['receipt']):
            context['warmup_end_ms'], context['end_ms'] = 40000, 122000
        args['collection_status'] = 'timeout'
        result = qualify_capture(**args, sources=sources)
        diagnostics = result['row_diagnostics']
        self.assertEqual(result['collection_status'], 'timeout')
        self.assertEqual(result['timing']['status'], 'complete')
        self.assertEqual(diagnostics['observed_benchmark_windows'], 2)
        self.assertEqual(diagnostics['rejected_benchmark_windows'], 1)
        self.assertEqual(diagnostics['rejected_gpu_missing_samples'], 7)
        self.assertEqual(diagnostics['rejected_gpu_invalid_samples'], 0)
        self.assertEqual(diagnostics['boundary_crossing_benchmark_windows'], 1)
        self.assertEqual(diagnostics['boundary_crossing_gpu_invalid_samples'], 3)

    def test_cli_dry_run_never_creates_and_published_result_never_replaces(self):
        args, sources = capture()
        with tempfile.TemporaryDirectory() as scratch, redirect_stdout(StringIO()), redirect_stderr(StringIO()):
            root = Path(scratch)
            manifest = root / 'capture.json'
            output = root / 'result.json'
            manifest.write_text(json.dumps(args), encoding='utf-8')
            for role, data in sources.items():
                (root / role).write_bytes(data)
            argv = ['--input', str(manifest), '--gfx', str(root/'gfx'),
                    '--openxr', str(root/'openxr'), '--output', str(output)]
            original = set(root.iterdir())
            self.assertEqual(main(argv + ['--dry-run']), 0)
            self.assertEqual(set(root.iterdir()), original)
            self.assertEqual(main(argv), 0)
            payload = output.read_bytes()
            self.assertEqual(main(argv), 2)
            self.assertEqual(output.read_bytes(), payload)
            self.assertNotIn(b'private-owner', payload)
            self.assertNotIn(b'creation_ticks', payload)


def self_test():
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(BenchmarkQualificationTests)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
