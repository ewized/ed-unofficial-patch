"""Offline fixtures for the EDVR capture qualification tool."""

from contextlib import redirect_stderr, redirect_stdout
from copy import deepcopy
import hashlib
from io import StringIO
import json
from pathlib import Path
import tempfile
import unittest

from edvr_benchmark import main, produce_qualification, qualify_capture


def digest(data):
    return hashlib.sha256(data).hexdigest()


def packet(sequence, time_ms, native, generated, repeated, session_key='a'):
    return dict(kind='presentation', now_ms=time_ms, captured_at_ms=time_ms, fresh=True,
                active=True, available=True, sequence=sequence, session_key=session_key, native=native,
                generated=generated, repeated=repeated)


def window(start=1000, invalid=0, missing=0):
    return dict(kind='benchmark', start_ms=start, end_ms=start+30000, sample_ms=30000,
                finished_ms=start+32000, drain_ms=2000, status='completed',
                scope=1, cpu={'p50_ms': 1}, gpu={'p50_ms': 2},
                cpu_valid=20, gpu_valid=20, cpu_invalid=0, cpu_missing=0,
                gpu_invalid=invalid, gpu_missing=missing, aa='off', model='k', build='a'*8,
                input_dimensions=[2000]*4, output_dimensions=[2000]*4,
                eligible=not (invalid or missing))


def capture(records=None, label='fixture'):
    records = deepcopy(records if records is not None else [window()])
    sources = {}
    by_role = {'gfx': [], 'openxr': []}
    for item in records:
        kind = item['kind']
        role = 'openxr' if kind.startswith('runtime_') or kind == 'fg_lifecycle' else 'gfx'
        by_role[role].append(item)
    for role, items in by_role.items():
        lines = [f'{role} {label} line {line}' for line in range(1, len(items)+1)]
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


def bound_capture(args, sources, *, aggregate_records=None, qualify_report=None):
    if aggregate_records is None:
        aggregate_records = lambda records, run: ({}, [])
    if qualify_report is None:
        qualify_report = lambda report, row: args['qualification']
    envelope = produce_qualification(args['run'], args['row'], args['records'], args['receipt'], sources,
                                     aggregate_records=aggregate_records, qualify_report=qualify_report)
    return args | {'qualification': envelope}


def qualify_fixture(args, sources):
    return qualify_capture(**bound_capture(args, sources), sources=sources)


def lifecycle(**fields):
    return dict(kind='fg_lifecycle', admission_schema=1, estimated_at_ms=3000,
                row_timestamp_qualified=True, generation=1, config=2, history=3, state=1,
                last_failure=2, requested_multiplier=2, active_multiplier=0, source_pairs=0,
                generated_pairs=0, refused_pairs=0, latest_source=0, active_sequence=10,
                displayed_endpoint=10, native_presented=10, generated_presented=0, refusal_mask=0,
                admission=2, enabled=1, scene_ready=1, loading=0, menu=0, caller_fresh=1,
                state_valid=1, generation_valid=1, config_valid=1) | fields


def admission_group(record, namespace, **summary):
    observation = {key: value for key, value in record.items() if key not in (
        'kind', 'admission_schema', 'estimated_at_ms', 'row_timestamp_qualified', 'line_number',
    )}
    observation['at_ms'] = record['estimated_at_ms']
    return dict(namespace=namespace, observation_count=1, kept_count=1,
                observations_truncated=False, observations=[observation], **summary)


def admission_details(**groups):
    return dict(schema=1, observation_scope='sparse_diagnostics_not_failure_rates', **groups)


def rehash_envelope(args, sources):
    envelope = args['qualification']
    context = {key: args[key] for key in ('run', 'row', 'records', 'receipt')}
    context.update(schema=2, qualification=envelope['qualification'],
                   sources={role: dict(sha256=digest(data), bytes=len(data)) for role, data in sources.items()})
    envelope['binding'] = digest(json.dumps(context, sort_keys=True, separators=(',', ':'),
                                           ensure_ascii=True, allow_nan=False).encode())


class BenchmarkQualificationTests(unittest.TestCase):
    def test_same_treatment_capture_envelopes_cannot_be_swapped(self):
        captures = []
        for label in ('baseline', 'candidate'):
            args, sources = capture([window(), lifecycle()], label=label)
            args['run']['fg'] = args['row']['frame_generation'] = args['qualification']['requested']['fg'] = '2x'
            args['receipt']['session_id'] = label
            captures.append((bound_capture(args, sources), sources))
        for (genuine, sources), (other, _) in (captures, captures[::-1]):
            self.assertEqual(qualify_capture(**genuine, sources=sources)['requested']['fg'], '2x')
            mixed = genuine | {'qualification': other['qualification']}
            with self.assertRaisesRegex(ValueError, 'Qualification binding differs'):
                qualify_capture(**mixed, sources=sources)

    def test_bound_envelope_rejects_legacy_unknown_and_modified_context(self):
        args, sources = capture()
        for key in ('target_before', 'target_after'):
            args['receipt'][key]['creation_ticks'] = 2**53
        with self.assertRaisesRegex(ValueError, 'Bound qualification schema 2 required'):
            qualify_capture(**args, sources=sources)
        bound = bound_capture(args, sources)
        changes = (
            ('run', 'target_windows', 2),
            ('row', 'expected_output_dimensions', [2100]*4),
            ('receipt', 'session_id', 'other-session'),
            ('record', 'model', 'other'),
            ('qualification', 'reasons', ['changed']),
        )
        for part, key, value in changes:
            with self.subTest(part=part):
                modified = deepcopy(bound)
                part_value = (modified['records'][0] if part == 'record' else
                              modified['qualification']['qualification'] if part == 'qualification' else modified[part])
                part_value[key] = value
                with self.assertRaisesRegex(ValueError, 'Qualification binding differs'):
                    qualify_capture(**modified, sources=sources)
        modified = deepcopy(bound)
        for key in ('target_before', 'target_after'):
            modified['receipt'][key]['creation_ticks'] = 2**53 + 1
        with self.assertRaisesRegex(ValueError, 'Qualification binding differs'):
            qualify_capture(**modified, sources=sources)
        modified['qualification']['schema'] = 3
        with self.assertRaisesRegex(ValueError, 'Bound qualification schema 2 required'):
            qualify_capture(**modified, sources=sources)
        modified = deepcopy(bound)
        modified['qualification']['qualification']['measurement'] = float('nan')
        with self.assertRaisesRegex(ValueError, 'Out of range float'):
            qualify_capture(**modified, sources=sources)

    def test_producer_recomputes_and_preserves_inputs(self):
        args, sources = capture([window(), packet(1, 2000, 1, 0, 0), packet(5, 3000, 5, 0, 0)])
        before = deepcopy(args)
        calls = []

        def aggregate_records(records, run):
            calls.append('aggregate')
            return {'native_count': records[-1]['native'] - records[-2]['native']}, []

        def qualify_report(report, row):
            calls.append('qualify')
            self.assertEqual(report['schema'], 1)
            self.assertEqual(report['run'], before['run'])
            self.assertEqual(report['records'], before['records'])
            self.assertEqual(row, before['row'])
            return before['qualification'] | {'native_count': report['aggregate']['native_count']}

        bound = bound_capture(args, sources, aggregate_records=aggregate_records, qualify_report=qualify_report)
        self.assertEqual(bound['qualification']['qualification']['native_count'], 4)
        self.assertEqual(args, before)
        self.assertEqual(bound, bound_capture(args, sources, aggregate_records=aggregate_records,
                                              qualify_report=qualify_report))
        qualify_capture(**bound, sources=sources)
        self.assertEqual(calls, ['aggregate', 'qualify'] * 2)

        def mutate_records(records, run):
            records[0]['eligible'] = False
            return {}, []

        with self.assertRaisesRegex(ValueError, 'callbacks changed the normalized inputs'):
            bound_capture(args, sources, aggregate_records=mutate_records, qualify_report=qualify_report)
        self.assertEqual(args, before)

    def test_rehashed_admission_observation_still_requires_settled_source_record(self):
        args, sources = capture([window(), lifecycle(), lifecycle(estimated_at_ms=999, state=2),
                                 lifecycle(estimated_at_ms=3200, row_timestamp_qualified=False)])
        fg = args['qualification']['treatments']['fg']
        fg['admission_details'] = admission_details(
            fg_lifecycle=admission_group(args['records'][1], 'runtime_lifecycle_v1'))
        bound = bound_capture(args, sources)
        for record, error in ((args['records'][2], 'outside the settled row'),
                              (args['records'][3], 'no matching settled record'),
                              (args['records'][1] | {'state': 7}, 'no matching settled record'),
                              (args['records'][1] | {'estimated_at_ms': 3100}, 'no matching settled record'),
                              (args['records'][1] | {'source_line_sha256': 'f'*64}, 'no matching settled record'),
                              (args['records'][1] | {'source_sha256': digest(sources['gfx'])}, 'source differs')):
            with self.subTest(error=error, record=record['estimated_at_ms']):
                modified = deepcopy(bound)
                details = modified['qualification']['qualification']['treatments']['fg']['admission_details']
                details['fg_lifecycle'] = admission_group(record, 'runtime_lifecycle_v1')
                rehash_envelope(modified, sources)
                with self.assertRaisesRegex(ValueError, error):
                    qualify_capture(**modified, sources=sources)

    def test_unsupported_fg_row_does_not_imply_aa_or_vrs_incompatibility(self):
        args, sources = capture([])
        args['row'].update(nominal_mode='quality', vrs='on', frame_generation='3x',
                           supported=False, apply_allowed=False)
        args['run'].update(aa='quality', vrs=True, fg='3x')
        args['qualification']['requested'] = dict(aa='quality', vrs='on', fg='3x')
        result = qualify_fixture(args, sources)
        self.assertEqual(result['collection_status'], 'unsupported')
        self.assertEqual(result['timing']['status'], 'unsupported')
        self.assertEqual(result['features'], {
            'aa': {'status': 'unavailable', 'reason': 'row_not_applied'},
            'vrs': {'status': 'unavailable', 'reason': 'row_not_applied'},
            'fg': {'status': 'unsupported', 'reason': 'fg_3x_not_implemented'},
        })

    def test_counter_specific_coverage_and_total_common_span(self):
        args, sources = capture([window(),
                                 packet(1, 2000, 10, 0, 5), packet(5, 3000, 13, 0, 6),
                                 packet(14, 4000, 20, None, 8), packet(22, 5000, 27, 0, 9)])
        args['qualification']['treatments']['fg']['status'] = 'active'
        result = qualify_fixture(args, sources)
        submission = result['submission']
        self.assertEqual(submission['rates_per_second'], dict(native=17/3, generated=0, repeated=4/3, total=4))
        self.assertEqual(submission['known_counter_coverage']['generated'],
                         dict(seconds=1, intervals=1, fraction_of_observed=1/3, fraction_of_row=1/99))
        self.assertEqual(result['features']['fg']['status'], 'partial')
        self.assertIsNone(result['features']['fg']['generated_observed'])
        self.assertEqual(result['timing']['status'], 'complete')

        args['qualification']['treatments']['fg']['status'] = 'inconsistent'
        result = qualify_fixture(args, sources)
        self.assertEqual(result['features']['fg']['status'], 'inconsistent')

    def test_unreported_counter_regressions_invalidate_delivery_interval(self):
        for counter in ('empty', 'failed', 'background'):
            with self.subTest(counter=counter):
                before = dict(empty=0, failed=0, background=0) | {counter: 5}
                after = dict(empty=0, failed=12, background=0) | {counter: 2}
                if counter == 'failed':
                    after['empty'] = 12
                args, sources = capture([packet(20, 2000, 10, 5, 0) | before,
                                         packet(40, 3000, 20, 6, 0) | after])
                submission = qualify_fixture(args, sources)['submission']
                self.assertEqual(submission['observed_intervals'], 0)
                self.assertEqual(submission['counts'], dict(native=None, generated=None, repeated=None, total=None))

    def test_unknown_and_zero_are_distinct(self):
        args, sources = capture([window(), packet(1, 2000, 1, None, 0), packet(4, 3000, 4, None, 0)])
        result = qualify_fixture(args, sources)
        self.assertEqual(result['submission']['rates_per_second'],
                         dict(native=3, generated=None, repeated=0, total=None))
        self.assertIsNone(result['features']['fg']['generated_observed'])
        self.assertEqual(result['features']['fg']['status'], 'unavailable')

    def test_selected_multiplier_does_not_prove_generation(self):
        args, sources = capture([
            packet(1, 2000, 1, 0, 0) | {'selected_multiplier': 2},
            packet(7, 3000, 4, 0, 3) | {'selected_multiplier': 2},
        ])
        result = qualify_fixture(args, sources)
        self.assertEqual(result['features']['fg']['status'], 'expected_inactive')
        self.assertIs(result['features']['fg']['generated_observed'], False)
        self.assertEqual(result['submission']['rates_per_second']['total'], 6)
        args['qualification']['treatments']['fg']['status'] = 'active'
        result = qualify_fixture(args, sources)
        self.assertEqual(result['features']['fg']['status'], 'inconsistent')

    def test_inactive_unavailable_and_stale_packets_break_coverage(self):
        for invalid in ({'active': 0}, {'available': False}, {'captured_at_ms': 3500, 'now_ms': 5501}):
            with self.subTest(invalid=invalid):
                args, sources = capture([
                    packet(1, 2000, 1, 0, None), packet(5, 3000, 5, 0, None),
                    packet(25, 5000, 15, 10, None) | invalid,
                    packet(45, 6000, 35, 10, None), packet(50, 7000, 40, 10, None),
                ])
                result = qualify_fixture(args, sources)['submission']
                self.assertEqual(result['counts'], dict(native=9, generated=0, repeated=None, total=None))
                self.assertEqual(result['observed_intervals'], 2)
                self.assertEqual(result['observed_seconds'], 2)

    def test_camera_and_backend_evidence_survive_status_override_without_aliasing(self):
        camera = dict(kind='fg_camera_refusal', admission_schema=1, estimated_at_ms=3100,
                      row_timestamp_qualified=True, sequence=10, eye=0, upscaler_slot=1,
                      camera_mask=49, camera_provenance=4, scene_frame=8, head_frame=9, eyes_frame=10,
                      selected_eye=-1, previous_eye=1, draws=7, input_width=1100, input_height=1100)
        source = dict(kind='fg_source_refusal', admission_schema=1, estimated_at_ms=3050,
                      row_timestamp_qualified=True, producer_reason=5, sequence=10, history=3, refused_count=2)
        args, sources = capture([window(), camera, source, lifecycle()])
        _, camera, source, runtime = args['records']
        original = args['qualification']['treatments']['fg']
        details = admission_details(
            fg_camera_refusal=admission_group(camera, 'producer_camera_v1', camera_masks=[49]),
            fg_source_refusal=admission_group(source, 'producer_source_v1', producer_reason_codes=[5]),
            fg_lifecycle=admission_group(runtime, 'runtime_lifecycle_v1', admission_values=[2]),
        )
        original.update(reasons=['no_observed_generated_submission_camera_refusal_observed'],
                        generated_count=0, activation_observed=False,
                        backend_failure_codes=[5], admission_details=details)
        bound = bound_capture(args, sources)
        result = qualify_capture(**bound, sources=sources)['features']['fg']
        self.assertEqual(result['status'], 'unavailable')
        self.assertIsNone(result['generated_count'])
        self.assertIsNone(result['activation_observed'])
        self.assertEqual(result['reasons'], original['reasons'])
        self.assertEqual(result['backend_failure_codes'], [5])
        self.assertEqual(result['admission_details'], details)
        self.assertEqual(result['admission_details']['fg_lifecycle']['namespace'], 'runtime_lifecycle_v1')
        result['admission_details']['fg_camera_refusal']['camera_masks'].clear()
        self.assertEqual(bound['qualification']['qualification']['treatments']['fg']['admission_details'], details)
        self.assertEqual(original['admission_details']['fg_camera_refusal']['camera_masks'], [49])
        self.assertEqual(original['status'], 'expected_inactive')

    def test_reused_session_or_reordered_packets_cannot_inflate_coverage(self):
        args, sources = capture([packet(1, 2000, 1, 0, 0), packet(11, 4000, 11, 0, 0),
                                 packet(1, 3000, 90, 30, 20, 'b') | {'now_ms': 4100},
                                 packet(30, 5000, 100, 40, 29, 'b'),
                                 packet(36, 6000, 106, 40, 29, 'b')])
        result = qualify_fixture(args, sources)
        self.assertEqual(result['submission']['counts']['native'], 16)
        self.assertEqual(result['submission']['observed_seconds'], 3)

    def test_same_build_reused_pid_birth_or_session_change_refused(self):
        args, sources = capture()
        args = bound_capture(args, sources)
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
        args = bound_capture(args, sources)
        with self.assertRaisesRegex(ValueError, 'hash or size differs'):
            qualify_capture(**args, sources=sources | {'gfx': sources['gfx'] + b'other'})
        args['records'][0]['line_number'] += 1
        with self.assertRaisesRegex(ValueError, 'Record line differs'):
            qualify_capture(**args, sources=sources)

    def test_native_equal_but_wrong_and_runtime_unknown_input_remain_distinct(self):
        args, sources = capture([window(), dict(kind='runtime_cycle', input_dimensions=None,
                                                input_status='unavailable', raw_input_dimensions=[0]*4,
                                                eligible=True, aggregate_admitted=True)])
        result = qualify_fixture(args, sources)
        self.assertEqual(result['features']['aa']['status'], 'matched')
        self.assertEqual(result['runtime_input_metadata'], dict(known_windows=0, unavailable_windows=1))
        args['row']['expected_output_dimensions'] = [2200]*4
        result = qualify_fixture(args, sources)
        self.assertEqual(result['features']['aa']['status'], 'wrong_mode')
        self.assertEqual(result['timing']['status'], 'complete')
        self.assertFalse(result['physical_scanout_qualified'])

    def test_dlss_one_wrong_eye_cannot_pass_agreement(self):
        measured = window() | {'aa': 'dlss', 'input_dimensions': [1100]*4,
                               'output_dimensions': [2000, 2000, 1900, 2000]}
        setup = dict(kind='dlss_mode', estimated_at_ms=900, row_timestamp_qualified=False)
        args, sources = capture([setup, measured])
        args['run']['aa'] = args['row']['nominal_mode'] = args['qualification']['requested']['aa'] = 'balanced'
        args['row']['expected_input_dimensions'] = [1100]*4
        result = qualify_fixture(args, sources)
        self.assertEqual(result['features']['aa']['status'], 'wrong_mode')

    def test_history_and_unsplittable_drain_do_not_count_as_row_faults(self):
        args, sources = capture([window(1000, invalid=20), window(40000, missing=7),
                                 window(71000), window(90500, invalid=3)])
        for context in (args['run'], args['receipt']):
            context['warmup_end_ms'], context['end_ms'] = 40000, 122000
        args['collection_status'] = 'timeout'
        result = qualify_fixture(args, sources)
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
        args = bound_capture(args, sources)
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
