#!/usr/bin/env python3
"""Collect background-observed SteamVR timing history for an explicitly selected scene.

Requires an explicit absolute OpenVR DLL path; runtime discovery belongs to the
existing helper. No runtime installation, scene submission, or setting changes.
Fields follow Valve's BSD-3-Clause SDK: src/d3d11/openvr026_official/openvr.h,
Compositor_FrameTiming and IVRCompositor; retained license: LICENSE.txt there.
C FnTable slots verified against Valve commit 0924064316de3effbcd1acf1e309182a2deb1c05,
headers/openvr_capi.h. These records do not prove native game cadence, generated
frame cadence, or native 1% lows. --dry-run writes nothing and loads no library.
Index skips are descriptive, not collection-loss counts. Qualification checks
overlap between successive raw history windows; batches.jsonl preserves that evidence.
"""
import argparse
import ctypes as ct
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import statistics
import sys
import tempfile
import time
import unittest
from unittest import mock


ABI_SOURCE = 'https://github.com/ValveSoftware/openvr/blob/0924064316de3effbcd1acf1e309182a2deb1c05/headers/openvr_capi.h'
FRESHNESS_ALLOWANCE = .25  # Five 50ms polling periods; a coverage bound, not a frame-rate target.
GPU_CPU_FIELDS = (
    'm_flPreSubmitGpuMs', 'm_flPostSubmitGpuMs', 'm_flTotalRenderGpuMs',
    'm_flCompositorRenderGpuMs', 'm_flCompositorRenderCpuMs', 'm_flCompositorIdleCpuMs',
    'm_flClientFrameIntervalMs', 'm_flPresentCallCpuMs', 'm_flWaitForPresentCpuMs', 'm_flSubmitFrameMs',
    'm_flWaitGetPosesCalledMs', 'm_flNewPosesReadyMs', 'm_flNewFrameReadyMs',
    'm_flCompositorUpdateStartMs', 'm_flCompositorUpdateEndMs', 'm_flCompositorRenderStartMs')
SEMANTICS = dict(zip(GPU_CPU_FIELDS, (
    'Scene GPU work submitted between WaitGetPoses and second Submit; may include other processes.',
    'Additional application GPU work, for example companion window.',
    'GPU span from work submitted immediately after present through end of compositor work.',
    'GPU distortion, chaperone, overlay and other compositor rendering.',
    'CPU time submitting compositor rendering work.', 'CPU time waiting for running start; not application CPU work.',
    'Interval between WaitGetPoses calls; not proof of native game or generated frame cadence.',
    'CPU time blocked in present.', 'CPU spin-wait for frame index change.', 'Time in IVRCompositor::Submit.',
    'WaitGetPoses call offset relative to m_flSystemTimeInSeconds.',
    'New poses ready offset relative to m_flSystemTimeInSeconds.',
    'Second Submit offset relative to m_flSystemTimeInSeconds.',
    'Compositor update start offset relative to m_flSystemTimeInSeconds.',
    'Compositor update end offset relative to m_flSystemTimeInSeconds.',
    'Compositor render start offset relative to m_flSystemTimeInSeconds.')))
SEMANTICS['m_flTransferLatencyMs'] = 'SDK compositor transfer-latency field; reported without reinterpretation.'


class Pose(ct.Structure):
    _pack_ = 8
    _fields_ = [('mDeviceToAbsoluteTracking', (ct.c_float * 4) * 3), ('vVelocity', ct.c_float * 3),
                ('vAngularVelocity', ct.c_float * 3), ('eTrackingResult', ct.c_int32),
                ('bPoseIsValid', ct.c_bool), ('bDeviceIsConnected', ct.c_bool)]


class FrameTiming(ct.Structure):
    _pack_ = 8
    _fields_ = [(name, ct.c_uint32) for name in ('m_nSize', 'm_nFrameIndex', 'm_nNumFramePresents',
                'm_nNumMisPresented', 'm_nNumDroppedFrames', 'm_nReprojectionFlags')]
    _fields_ += [('m_flSystemTimeInSeconds', ct.c_double)]
    _fields_ += [(name, ct.c_float) for name in GPU_CPU_FIELDS]
    _fields_ += [('m_HmdPose', Pose), ('m_nNumVSyncsReadyForUse', ct.c_uint32),
                 ('m_nNumVSyncsToFirstView', ct.c_uint32), ('m_flTransferLatencyMs', ct.c_float)]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def plain(value):
    """Serialize every SDK field; represent invalid nonfinite floats explicitly."""
    if isinstance(value, ct.Structure):
        return {name: plain(getattr(value, name)) for name, _ in value._fields_}
    if isinstance(value, ct.Array):
        return [plain(item) for item in value]
    if isinstance(value, float) and not math.isfinite(value):
        return str(value)
    return value


class Runtime:
    """Own exactly one successful VRApplication_Background initialization/shutdown."""
    def __init__(self, dll):
        self.path, self.initialized = dll, False

    def __enter__(self):
        require(sys.platform == 'win32' and ct.sizeof(ct.c_void_p) == 8, 'Windows x64 Python required')
        require(ct.sizeof(FrameTiming) == 192 and FrameTiming.m_HmdPose.offset == 96, 'Unsupported ctypes timing ABI')
        self.library = ct.CDLL(str(self.path), winmode=0x1100)
        init = self.library.VR_InitInternal2
        init.argtypes, init.restype = [ct.POINTER(ct.c_int32), ct.c_int32, ct.c_char_p], ct.c_uint32
        self.shutdown = self.library.VR_ShutdownInternal
        self.shutdown.argtypes, self.shutdown.restype = [], None
        self.generic = self.library.VR_GetGenericInterface
        self.generic.argtypes, self.generic.restype = [ct.c_char_p, ct.POINTER(ct.c_int32)], ct.c_void_p
        error = ct.c_int32()
        init(ct.byref(error), 3, None)  # VRApplication_Background, no scene/focus submission.
        require(error.value == 0, 'OpenVR background initialization failed: ' + str(error.value))
        self.initialized = True
        try:
            system = self.table('IVRSystem_026')
            compositor = self.table('IVRCompositor_029')
            overlay = self.table('IVROverlay_028')  # Unknown visibility must fail, not imply hidden.
            self.scene = self.function(compositor, 24, ct.c_uint32)
            self.timings = self.function(compositor, 11, ct.c_uint32, ct.POINTER(FrameTiming), ct.c_uint32)
            self.dashboard = self.function(overlay, 68, ct.c_bool)
            version = self.function(system, 49, ct.c_char_p)
            recommended = self.function(system, 0, None, ct.POINTER(ct.c_uint32), ct.POINTER(ct.c_uint32))
            adapter = self.function(system, 8, None, ct.POINTER(ct.c_int32))
            width, height, gpu = ct.c_uint32(), ct.c_uint32(), ct.c_int32(-1)
            recommended(ct.byref(width), ct.byref(height))
            adapter(ct.byref(gpu))
            self.metadata = {'runtime_version': (version() or b'').decode('utf-8', errors='replace'),
                             'recommended_eye_size': [width.value, height.value], 'dxgi_adapter_index': gpu.value,
                             'interfaces': ['IVRSystem_026', 'IVRCompositor_029', 'IVROverlay_028'],
                             'application_type': 'VRApplication_Background'}
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def table(self, name):
        error = ct.c_int32()
        address = self.generic(('FnTable:' + name).encode('ascii'), ct.byref(error))
        require(address and error.value == 0, 'Required OpenVR interface unavailable: ' + name)
        return ct.cast(address, ct.POINTER(ct.c_void_p))

    @staticmethod
    def function(table, index, result, *args):
        require(bool(table[index]), 'Null OpenVR function pointer')
        return ct.WINFUNCTYPE(result, *args)(table[index])  # Windows stdcall; no C++ this.

    def history(self):
        frames = (FrameTiming * 64)()
        for frame in frames:
            frame.m_nSize = ct.sizeof(FrameTiming)  # SDK requires only first; initialize all defensively.
        count = self.timings(frames, 64)
        require(count <= 64, 'Runtime returned oversized timing history')
        return list(frames[:count])

    def __exit__(self, *_):
        if self.initialized:
            self.initialized = False
            self.shutdown()


def percentile(values, fraction):
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower, upper = math.floor(position), math.ceil(position)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


class Collector:
    """Use ascending SDK history, skipping its newest entry and pre-window history."""
    def __init__(self):
        self.frames, self.rejected = [], []
        self.batches = []
        self.previous_batch = None
        self.sdk_start = self.sdk_latest = self.last_progress_observed = None
        self.progress_updates = self.misaligned_batches = 0
        self.freshness = {'evaluated': False, 'ok': False, 'errors': ['Collection window not finalized']}
        self.possible_overruns = self.empty_batches = self.batch_discontinuities = 0
        self.last = None
        self.gaps = self.duplicates = self.incomplete = self.discontinuities = self.wraps = 0

    def record_batch(self, history, observed, prime=False):
        """Audit raw SDK window overlap without assuming consecutive returned indices.

        Each compact row stores raw index, system timestamp and presentation count.
        An absent overlap is a possible history overrun, not a count of lost frames.
        Runtime non-presented slots and repeated presentation do not invalidate overlap.
        """
        self.freshness = {'evaluated': False, 'ok': False, 'errors': ['Collection window not finalized']}
        rows = [[frame.m_nFrameIndex, plain(frame.m_flSystemTimeInSeconds), frame.m_nNumFramePresents]
                for frame in history]
        previous = self.previous_batch
        common = sorted({row[0] for row in rows} & {row[0] for row in previous}) if previous else []
        continuity = 'prime' if prime else 'overlap'
        if not rows:
            self.empty_batches += 1
            continuity = 'empty_history'
        elif previous:
            before, after = previous[-1], rows[-1]
            if (type(before[1]) is float and type(after[1]) is float and
                    (after[1] < before[1] or (after[1] == before[1] and after[0] != before[0]))):
                self.batch_discontinuities += 1
                continuity = 'raw_history_time_discontinuity'
            elif not common:
                self.possible_overruns += 1
                continuity = 'no_overlap_possible_history_overrun'
        elif not prime:
            continuity = 'missing_prime'
            self.possible_overruns += 1
        progress = lag = None
        if rows and type(rows[-1][1]) is float and math.isfinite(rows[-1][1]):
            stamp = rows[-1][1]
            if prime:
                self.sdk_start = self.sdk_latest = stamp
                self.last_progress_observed = 0
            elif self.sdk_start is not None:
                if self.sdk_latest is not None and stamp > self.sdk_latest:
                    self.progress_updates += 1
                    self.last_progress_observed = observed
                self.sdk_latest = stamp
                progress = stamp-self.sdk_start
                lag = observed-progress
                if abs(lag) > FRESHNESS_ALLOWANCE:
                    self.misaligned_batches += 1
        self.batches.append({'phase': 'prime' if prime else 'poll', 'observed_seconds': observed,
                             'continuity': continuity, 'overlap_indices': common,
                             'sdk_progress_seconds': progress, 'monotonic_minus_sdk_progress_seconds': lag,
                             'previous_newest': previous[-1] if previous else None,
                             'entry_fields': ['m_nFrameIndex','m_flSystemTimeInSeconds','m_nNumFramePresents'],
                             'entries': rows})
        if rows:
            self.previous_batch = rows

    def continuity_ok(self):
        return bool(self.frames) and not (self.possible_overruns or self.empty_batches or
                    self.batch_discontinuities or self.rejected or self.discontinuities)

    def finish(self, requested, elapsed):
        """Compare relative SDK timestamps with this collection's monotonic window.

        No common clock epoch is assumed. Both clocks are anchored at the initial
        history query; bounds qualify observable progress and selected-time coverage,
        never native application cadence or a stable gameplay scene.
        """
        errors = []
        allowance = FRESHNESS_ALLOWANCE
        window = max(requested,elapsed)
        frames = self.frames
        span = frames[-1]['timing']['m_flSystemTimeInSeconds']-frames[0]['timing']['m_flSystemTimeInSeconds'] if len(frames) >= 2 else 0
        required_span = max(0,window-2*allowance)
        raw_progress = self.sdk_latest-self.sdk_start if self.sdk_latest is not None and self.sdk_start is not None else None
        progress_age = elapsed-self.last_progress_observed if self.last_progress_observed is not None else None
        first_observed = frames[0]['observed_seconds'] if frames else None
        last_age = elapsed-frames[-1]['observed_seconds'] if frames else None
        first_offset = frames[0]['timing']['m_flSystemTimeInSeconds']-self.sdk_start if frames and self.sdk_start is not None else None
        last_sdk_age = elapsed-(frames[-1]['timing']['m_flSystemTimeInSeconds']-self.sdk_start) if frames and self.sdk_start is not None else None
        if len(frames) < 2 or self.progress_updates < 2:
            errors.append('Insufficient distinct selected samples or SDK progress observations')
        if first_observed is None or first_observed > allowance or first_offset is None or not 0 <= first_offset <= allowance:
            errors.append('Selected SDK samples do not cover the collection start within allowance')
        if progress_age is None or not 0 <= progress_age <= allowance or last_age is None or not 0 <= last_age <= allowance:
            errors.append('SDK progress or selected samples are stale at collection end')
        if raw_progress is None or abs(raw_progress-elapsed) > allowance or last_sdk_age is None or abs(last_sdk_age) > allowance:
            errors.append('SDK timestamp progress does not track the monotonic collection window')
        if span+1e-6 < required_span:
            errors.append('Selected SDK timestamp span is shorter than the collection window allowance')
        if self.misaligned_batches:
            errors.append('Raw SDK progress diverged from monotonic time during polling')
        self.freshness = {'evaluated': True, 'ok': not errors, 'errors': errors,
                          'allowance_seconds': allowance, 'requested_seconds': requested, 'elapsed_seconds': elapsed,
                          'selected_sdk_span_seconds': span, 'required_selected_sdk_span_seconds': required_span,
                          'raw_sdk_progress_seconds': raw_progress, 'sdk_progress_updates': self.progress_updates,
                          'raw_progress_end_age_seconds': progress_age, 'first_sample_observed_seconds': first_observed,
                          'last_sample_end_age_seconds': last_age, 'first_sample_sdk_offset_seconds': first_offset,
                          'last_sample_sdk_end_age_seconds': last_sdk_age,
                          'misaligned_raw_batches': self.misaligned_batches}

    def coverage_ok(self):
        return self.continuity_ok() and self.freshness['evaluated'] and self.freshness['ok']

    def prime(self, history):
        self.record_batch(history, 0, prime=True)
        require(bool(history), 'No compositor history to establish collection boundary')
        newest = history[-1]
        require(math.isfinite(newest.m_flSystemTimeInSeconds) and newest.m_flSystemTimeInSeconds > 0,
                'Invalid initial compositor history boundary')
        self.last = (newest.m_nFrameIndex, newest.m_flSystemTimeInSeconds)

    def ingest(self, history, observed):
        self.record_batch(history, observed)
        if history:
            self.incomplete += 1  # Always exclude newest, even if some fields appear complete.
        for frame in history[:-1]:
            index, stamp = frame.m_nFrameIndex, frame.m_flSystemTimeInSeconds
            values = [getattr(frame, field) for field in (*GPU_CPU_FIELDS, 'm_flTransferLatencyMs')]
            if frame.m_nSize != 192 or not math.isfinite(stamp) or stamp <= 0 or not all(math.isfinite(v) for v in values):
                self.rejected.append({'reason': 'invalid_size_or_nonfinite_timing', 'timing': plain(frame)})
                continue
            if frame.m_nNumFramePresents == 0:
                self.incomplete += 1
                continue
            if any(value < 0 for value in values[:10]) or values[-1] < 0:
                self.rejected.append({'reason': 'negative_duration', 'timing': plain(frame)})
                continue
            delta = (index - self.last[0]) & 0xffffffff
            if delta == 0 or delta >= 0x80000000:
                if stamp > self.last[1]:
                    self.discontinuities += 1
                    self.rejected.append({'reason': 'frame_index_discontinuity', 'timing': plain(frame)})
                else:
                    self.duplicates += 1
                continue
            if stamp <= self.last[1]:
                self.discontinuities += 1
                self.rejected.append({'reason': 'system_time_discontinuity', 'timing': plain(frame)})
                continue
            self.gaps += delta - 1
            self.wraps += int(index < self.last[0])
            self.last = (index, stamp)
            self.frames.append({'observed_seconds': observed, 'timing': plain(frame)})

    def summary(self):
        frames = [record['timing'] for record in self.frames]
        metrics = {}
        for field in SEMANTICS:
            values = [frame[field] for frame in frames]
            if values:
                metrics[field] = {'median_ms': statistics.median(values), 'p95_ms': percentile(values, .95),
                                  'p99_ms': percentile(values, .99), 'sdk_semantics': SEMANTICS[field]}
        return {'sample_count': len(frames), 'metrics': metrics, 'frame_index_gaps': self.gaps,
                'frame_index_gap_scope': 'Index skips between accepted positive-presentation samples; not collection loss',
                'possible_history_overruns': self.possible_overruns, 'empty_history_batches': self.empty_batches,
                'raw_history_time_discontinuities': self.batch_discontinuities,
                'history_batch_count': len(self.batches),
                'freshness': self.freshness,
                'history_duplicates_ignored': self.duplicates, 'newest_or_incomplete_observations_skipped': self.incomplete,
                'invalid_records': len(self.rejected), 'timing_discontinuities': self.discontinuities,
                'counter_wraps': self.wraps,
                'sdk_num_frame_presents_sum': sum(f['m_nNumFramePresents'] for f in frames),
                'sdk_num_mispresented_sum': sum(f['m_nNumMisPresented'] for f in frames),
                'sdk_num_dropped_frames_sum': sum(f['m_nNumDroppedFrames'] for f in frames)}


def check_scene(runtime, expected):
    require(runtime.scene() == expected, 'Scene focus PID changed or differs from expected PID')
    require(runtime.dashboard() is False, 'SteamVR dashboard is visible or visibility is unknown')


def collect(runtime, expected, duration, collector, clock=time.monotonic, pause=time.sleep):
    check_scene(runtime, expected)
    initial = runtime.history()
    check_scene(runtime, expected)
    collector.prime(initial)
    start = clock()
    try:
        while clock() - start < duration:
            check_scene(runtime, expected)
            history = runtime.history()
            check_scene(runtime, expected)
            collector.ingest(history, clock() - start)
            pause(min(.05, max(0, duration - (clock() - start))))
        check_scene(runtime, expected)
    finally:
        elapsed = clock()-start
        collector.finish(duration,elapsed)
    return elapsed


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--openvr-dll', type=Path, help='Explicit absolute SteamVR openvr_api.dll')
    parser.add_argument('--expect-scene-pid', type=int)
    parser.add_argument('--duration', type=float, default=30)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args(argv)
    if args.self_test:
        return 0 if unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(Tests)).wasSuccessful() else 1
    try:
        require(args.openvr_dll is not None and args.openvr_dll.is_absolute() and args.openvr_dll.is_file(),
                '--openvr-dll must name an absolute existing DLL')
        require(args.expect_scene_pid is not None and args.expect_scene_pid > 0, '--expect-scene-pid must be positive')
        require(math.isfinite(args.duration) and 1 <= args.duration <= 300, '--duration must be 1..300 seconds')
        require(args.output is not None and not args.output.exists() and not args.output.is_symlink(), 'Output must be new')
        require(args.output.parent.is_dir(), 'Output parent must already exist')
        if args.dry_run:
            print('Would collect qualified SteamVR timing history; no DLL loaded or files written')
            return 0
        report = {'ok': False, 'qualified': False, 'errors': [], 'expected_scene_pid': args.expect_scene_pid,
                  'runtime_dll': str(args.openvr_dll), 'runtime_dll_sha256': hashlib.sha256(args.openvr_dll.read_bytes()).hexdigest(),
                  'requested_duration_seconds': args.duration, 'poll_interval_seconds': .05,
                  'scope': 'Background-observed SteamVR compositor SDK fields; no native FPS, native 1% low, AutoFG cadence or capture-latency inference',
                  'abi_source': ABI_SOURCE,
                  'sdk_fields_source': 'src/d3d11/openvr026_official/openvr.h; adjacent LICENSE.txt (BSD-3-Clause)',
                  'scene_validation_scope': 'Expected PID and hidden dashboard checked before and after every history query; between-check transitions cannot be observed',
                  'percentile_method': 'Linear interpolation at (count-1)*fraction'}
        args.output.mkdir()
        collector = Collector()
        try:
            with Runtime(args.openvr_dll) as runtime:
                report['runtime'] = runtime.metadata
                report['collection_started_utc'] = datetime.now(timezone.utc).isoformat()
                started = time.monotonic()
                try:
                    report['elapsed_seconds'] = collect(runtime, args.expect_scene_pid, args.duration, collector)
                finally:
                    report['collection_finished_utc'] = datetime.now(timezone.utc).isoformat()
                    report['operation_elapsed_seconds'] = time.monotonic() - started
            require(bool(collector.frames), 'No finalized timing records collected')
            report['ok'] = True
        except (Exception, KeyboardInterrupt) as error:
            report['errors'].append(str(error) or type(error).__name__)
        report.update(collector.summary())
        report['qualified'] = report['ok'] and collector.coverage_ok()
        report['qualification_scope'] = 'Overlapping raw SDK history windows, SDK progress/time-span freshness, valid selected samples and PID/dashboard guards; no stable-scene or native-cadence claim'
        if report['ok'] and not report['qualified']:
            report['errors'].append('History continuity, validity or SDK progress/window freshness failed; metrics are unqualified')
            report['errors'].extend(collector.freshness['errors'])
        for filename, records in (('frames.jsonl', collector.frames), ('rejected.jsonl', collector.rejected),
                                  ('batches.jsonl', collector.batches)):
            with (args.output / filename).open('x', encoding='utf-8') as stream:
                for record in records:
                    stream.write(json.dumps(record, allow_nan=False) + '\n')
        with (args.output / 'summary.json').open('x', encoding='utf-8') as stream:
            json.dump(report, stream, indent=2, allow_nan=False)
            stream.write('\n')
        print(str(args.output / 'summary.json'))
        return 0 if report['qualified'] else 1
    except (OSError, ValueError) as error:
        print('steamvr_benchmark: ' + str(error), file=sys.stderr)
        return 2


class Tests(unittest.TestCase):
    @staticmethod
    def frame(index, stamp=None):
        value = FrameTiming()
        value.m_nSize, value.m_nFrameIndex, value.m_nNumFramePresents = 192, index, 1
        value.m_flSystemTimeInSeconds = stamp if stamp is not None else index + 1
        value.m_flPreSubmitGpuMs = 4
        return value

    def test_abi(self):
        self.assertEqual(ct.sizeof(Pose), 80)
        self.assertEqual(ct.sizeof(FrameTiming), 192)
        for field, offset in (('m_flSystemTimeInSeconds', 24), ('m_flPreSubmitGpuMs', 32),
                              ('m_HmdPose', 96), ('m_nNumVSyncsReadyForUse', 176), ('m_flTransferLatencyMs', 184)):
            self.assertEqual(getattr(FrameTiming, field).offset, offset)

    def test_frozen_overlapping_history_cannot_qualify_a_completed_window(self):
        collector = Collector()
        collector.prime([self.frame(0,100)])
        frozen = [self.frame(0,100),self.frame(1,100.01),self.frame(2,100.02)]
        for observed in (.1,1,10,30):
            collector.ingest(frozen,observed)
        self.assertTrue(collector.continuity_ok())
        self.assertFalse(collector.coverage_ok())  # No finished-window evidence yet.
        collector.finish(30,30)
        self.assertFalse(collector.coverage_ok())
        self.assertEqual(len(collector.frames),1)
        self.assertGreater(collector.freshness['raw_progress_end_age_seconds'],29)
        self.assertTrue(any('stale' in reason for reason in collector.freshness['errors']))

    def test_live_stride_three_covers_sdk_time_then_end_freeze_is_rejected(self):
        collector = Collector()
        collector.prime([self.frame(0,100)])
        for tick in range(1,601):
            history = [self.frame(index*3,100+index*.05) for index in range(max(0,tick-2),tick+1)]
            for frame in history: frame.m_nNumFramePresents = 3
            collector.ingest(history,tick*.05)
        collector.finish(30,30)
        self.assertTrue(collector.coverage_ok(),collector.freshness)
        self.assertGreater(collector.gaps,0)  # Runtime spacing still is not collection loss.
        self.assertGreater(collector.freshness['selected_sdk_span_seconds'],29.5)
        collector.ingest(history,40)
        self.assertFalse(collector.coverage_ok())  # Appended observations invalidate the earlier window check.
        collector.finish(40,40)
        self.assertFalse(collector.coverage_ok())
        self.assertGreater(collector.freshness['raw_progress_end_age_seconds'],9)

    def test_completed_collection_runs_freshness_checks_and_rejects_clock_reset(self):
        now = [0.0]
        runtime = mock.Mock()
        runtime.scene.return_value,runtime.dashboard.return_value = 42,False
        def history():
            tick = round(now[0]/.05)
            return [self.frame(index*3,100+index*.05) for index in range(max(0,tick-2),tick+1)]
        runtime.history.side_effect = history
        collector = Collector()
        elapsed = collect(runtime,42,1,collector,clock=lambda:now[0],pause=lambda amount:now.__setitem__(0,now[0]+amount))
        self.assertTrue(collector.coverage_ok(),collector.freshness)
        self.assertAlmostEqual(elapsed,1)
        collector.ingest([self.frame(0,90),self.frame(1,90.05)],1.05)
        collector.finish(1,1.05)
        self.assertFalse(collector.coverage_ok())
        self.assertGreater(collector.batch_discontinuities,0)

    def test_finalized_dedup_gaps_wrap(self):
        collector = Collector()
        collector.prime([self.frame(0xfffffffe, 10)])
        collector.ingest([self.frame(0xfffffffe, 10), self.frame(0xffffffff, 11), self.frame(0, 12), self.frame(1, 13)], .1)
        collector.ingest([self.frame(0, 12), self.frame(2, 14), self.frame(3, 15)], .2)
        self.assertEqual([f['timing']['m_nFrameIndex'] for f in collector.frames], [0xffffffff, 0, 2])
        self.assertEqual((collector.wraps, collector.gaps, collector.duplicates), (1, 1, 2))

    def test_raw_overlap_accepts_stride_three_but_detects_window_overrun(self):
        def batch(indices):
            frames = [self.frame(index) for index in indices]
            for frame in frames:
                frame.m_nNumFramePresents = 3
            return frames
        collector = Collector()
        collector.prime(batch([0,3,6]))
        collector.ingest(batch([3,6,9,12]), .1)
        collector.ingest(batch([9,12,15]), .2)
        self.assertEqual([record['timing']['m_nFrameIndex'] for record in collector.frames],[9,12])
        self.assertEqual(collector.gaps,4)  # Descriptive SDK spacing, not four lost samples.
        self.assertTrue(collector.continuity_ok())
        self.assertEqual(collector.batches[1]['overlap_indices'],[3,6])
        self.assertEqual(collector.batches[1]['entries'],[[3,4.0,3],[6,7.0,3],[9,10.0,3],[12,13.0,3]])
        collector.ingest(batch([100,103,106]), .3)  # Previous 64-slot window could have been overrun.
        self.assertFalse(collector.continuity_ok())
        self.assertEqual(collector.possible_overruns,1)
        self.assertEqual(collector.batches[-1]['continuity'],'no_overlap_possible_history_overrun')

    def test_zero_presentation_slots_preserve_raw_window_continuity(self):
        collector = Collector()
        collector.prime([self.frame(0),self.frame(1)])
        batch = [self.frame(index) for index in range(1,6)]
        batch[1].m_nNumFramePresents = batch[2].m_nNumFramePresents = 0
        collector.ingest(batch,.1)
        self.assertEqual([row['timing']['m_nFrameIndex'] for row in collector.frames],[4])
        self.assertTrue(collector.continuity_ok())
        self.assertEqual(collector.batches[-1]['entries'][1:3],[[2,3.0,0],[3,4.0,0]])

    def test_raw_batch_empty_and_time_reset_are_unqualified(self):
        for batch in ([],[self.frame(0,.5),self.frame(1,.7)]):
            collector = Collector()
            collector.prime([self.frame(0,1)])
            collector.ingest([self.frame(0,1),self.frame(1,2),self.frame(2,3)],.1)
            self.assertTrue(collector.continuity_ok())
            collector.ingest(batch,.2)
            self.assertFalse(collector.continuity_ok())
            self.assertTrue(collector.empty_batches or collector.batch_discontinuities)

    def test_invalid_and_discontinuity(self):
        collector = Collector()
        collector.prime([self.frame(1)])
        invalid, negative, incomplete = self.frame(2), self.frame(3), self.frame(4)
        invalid.m_flTotalRenderGpuMs = float('nan')
        negative.m_flPreSubmitGpuMs = -1
        incomplete.m_nNumFramePresents = 0
        collector.ingest([invalid, negative, incomplete, self.frame(5, 1), self.frame(6)], .1)
        self.assertEqual(len(collector.frames), 0)
        self.assertEqual(len(collector.rejected), 3)
        self.assertEqual(collector.discontinuities, 1)
        json.dumps(collector.rejected, allow_nan=False)

    def test_summary(self):
        collector = Collector()
        collector.prime([self.frame(0)])
        values = [self.frame(i) for i in range(1, 6)]
        for index, value in enumerate(values): value.m_flPreSubmitGpuMs = index + 1
        collector.ingest(values, .1)
        metric = collector.summary()['metrics']['m_flPreSubmitGpuMs']
        self.assertEqual(metric['median_ms'], 2.5)
        self.assertAlmostEqual(metric['p95_ms'], 3.85)
        self.assertAlmostEqual(metric['p99_ms'], 3.97)

    def test_scene_dashboard(self):
        for pid, dashboard in ((1, False), (42, True), (42, None)):
            runtime = mock.Mock()
            runtime.scene.return_value, runtime.dashboard.return_value = pid, dashboard
            with self.assertRaises(ValueError): check_scene(runtime, 42)
        runtime.scene.return_value, runtime.dashboard.return_value = 42, False
        check_scene(runtime, 42)

    def test_scene_change_during_query(self):
        runtime = mock.Mock()
        runtime.dashboard.return_value = False
        runtime.scene.side_effect = [42, 42, 42, 43]
        runtime.history.side_effect = [[self.frame(0)], [self.frame(1), self.frame(2)]]
        ticks = iter([0, .1, .2])
        collector = Collector()
        with self.assertRaisesRegex(ValueError, 'PID changed'):
            collect(runtime, 42, 1, collector, clock=lambda: next(ticks), pause=lambda _: None)
        self.assertEqual(collector.frames, [])  # Do not ingest the batch spanning lost scene ownership.

    def test_shutdown_once(self):
        runtime = Runtime(Path('unused'))
        runtime.initialized = True
        runtime.shutdown = mock.Mock()
        runtime.__exit__(None, None, None)
        runtime.__exit__(None, None, None)
        runtime.shutdown.assert_called_once_with()

    def test_dry_run(self):
        with tempfile.TemporaryDirectory() as directory:
            dll = Path(directory) / 'openvr_api.dll'
            dll.write_bytes(b'fixture')
            with mock.patch.object(Runtime, '__enter__', side_effect=AssertionError('must not initialize')):
                result = main(['--openvr-dll', str(dll), '--expect-scene-pid', '42',
                               '--output', str(Path(directory) / 'new'), '--dry-run'])
            self.assertEqual(result, 0)
            self.assertEqual(list(Path(directory).iterdir()), [dll])


if __name__ == '__main__':
    sys.exit(main())
