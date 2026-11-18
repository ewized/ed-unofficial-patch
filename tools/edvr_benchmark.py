"""Qualify a captured EDVR benchmark row using the reviewed parser's records.

Acquisition and wire parsing stay with the controller. A capture receipt binds
its selected streams to the guarded process and row. A qualification envelope
binds the reviewed result to that capture when the controller computes it;
neither is proof that an untrusted producer parsed those bytes correctly.
Runtime phase records remain with the parser: no phase is added to another or
substituted for the raw DXGI Present measurement. See docs/benchmark-qualification.md.
"""

import argparse
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import re
import sys


SUBMISSION_COUNTERS = ('native', 'generated', 'repeated')
CUMULATIVE_COUNTERS = (*SUBMISSION_COUNTERS, 'empty', 'failed', 'background')
SOURCE_ROLES = ('gfx', 'openxr')
QUALIFICATION_SCHEMA = 2
ADMISSION_OBSERVATIONS = {
    'fg_source_refusal': ('gfx', 'producer_source_v1', (
        'producer_reason', 'sequence', 'history', 'refused_count',
    )),
    'fg_camera_refusal': ('gfx', 'producer_camera_v1', (
        'sequence', 'eye', 'upscaler_slot', 'camera_mask', 'camera_provenance',
        'scene_frame', 'head_frame', 'eyes_frame', 'selected_eye', 'previous_eye',
        'draws', 'input_width', 'input_height',
    )),
    'fg_lifecycle': ('openxr', 'runtime_lifecycle_v1', (
        'generation', 'config', 'history', 'state', 'last_failure', 'requested_multiplier',
        'active_multiplier', 'source_pairs', 'generated_pairs', 'refused_pairs', 'latest_source',
        'active_sequence', 'displayed_endpoint', 'native_presented', 'generated_presented',
        'refusal_mask', 'admission', 'enabled', 'scene_ready', 'loading', 'menu', 'caller_fresh',
        'state_valid', 'generation_valid', 'config_valid',
    )),
}
COLLECTION_STATUSES = ('complete', 'timeout', 'failed')
FEATURE_STATUSES = frozenset({
    'active', 'expected_inactive', 'inactive', 'inconsistent', 'matched',
    'native', 'off', 'partial', 'unavailable', 'unqualified', 'unsupported',
    'wrong_mode',
})
REQUESTS = {
    'aa': ('native', 'performance', 'balanced', 'quality'),
    'vrs': ('off', 'on'),
    'fg': ('off', 'auto', '2x', '3x', '4x'),
}


def _require(condition, message):
    if not condition:
        raise ValueError(message)


def _uint(value):
    return type(value) is int and value >= 0


def _flag(value):
    return value is True or type(value) is int and value == 1


def _hex(value, size):
    return isinstance(value, str) and re.fullmatch(r'[0-9a-f]{%d}' % size, value) is not None


def _digest(data):
    return hashlib.sha256(data).hexdigest()


def _require_json_keys(value):
    if isinstance(value, dict):
        _require(all(isinstance(key, str) for key in value), 'Capture JSON keys must be strings')
        for member in value.values():
            _require_json_keys(member)
    elif isinstance(value, (list, tuple)):
        for member in value:
            _require_json_keys(member)


def _canonical_json(value):
    _require_json_keys(value)
    return json.dumps(value, sort_keys=True, separators=(',', ':'),
                      ensure_ascii=True, allow_nan=False).encode('utf-8')


def _qualification_binding(run, row, records, qualification, receipt, fingerprints):
    bound = dict(schema=QUALIFICATION_SCHEMA, run=run, row=row, records=records,
                 qualification=qualification, receipt=receipt, sources=fingerprints)
    return _digest(_canonical_json(bound))


def _dimensions(value):
    return (isinstance(value, (list, tuple)) and len(value) == 4
            and all(_uint(item) and item > 0 for item in value))


def _bounds(run):
    start, end = run['warmup_end_ms'], run['end_ms']
    _require(_uint(start) and _uint(end) and start < end, 'Invalid settled row interval')
    return start, end


def _target(value):
    """Normalize the Windows process birth ticks without rounding."""
    _require(type(value) is dict and set(value) == {'pid', 'creation_ticks', 'image', 'session'},
             'Incomplete process identity')
    pid, birth, image, session = (value[key] for key in ('pid', 'creation_ticks', 'image', 'session'))
    if isinstance(birth, str) and re.fullmatch(r'[0-9]{1,20}', birth):
        birth = int(birth)
    _require(_uint(pid) and pid > 0 and _uint(birth) and birth > 0 and _uint(session)
             and isinstance(image, str) and bool(image.strip()), 'Invalid process identity')
    return pid, birth, image.casefold(), session


def _verify_capture(run, receipt, records, sources):
    """Check acquisition identity and pin record provenance to supplied bytes."""
    build = run['expected_build']
    _require(_hex(build, 40) and receipt['build'] == build and run['build_verified'] is True,
             'Exact verified build required')
    config = run['config_sha256']
    _require(_hex(config, 64) and receipt['config_sha256'] == config, 'Capture configuration differs')
    _require(_bounds(run) == _bounds(receipt), 'Capture interval differs')
    _require(_target(receipt['target_before']) == _target(receipt['target_after']),
             'Process identity changed during capture')
    _require(isinstance(receipt['session_id'], str) and bool(receipt['session_id'].strip()),
             'Owned capture session required')
    claimed = receipt['sources']
    _require(set(sources) == set(claimed) == set(SOURCE_ROLES), 'Both capture sources required')
    fingerprints, source_lines = {}, {}
    for role in SOURCE_ROLES:
        data = sources[role]
        _require(type(data) is bytes, 'Capture sources must contain bytes')
        fingerprint = {'sha256': _digest(data), 'bytes': len(data)}
        _require(type(claimed[role].get('bytes')) is int and claimed[role] == fingerprint,
                 'Capture source hash or size differs')
        fingerprints[role] = fingerprint
        source_lines[role] = data.decode('utf-8-sig', errors='replace').splitlines()
    for record in records:
        kind = record['kind']
        matches = [role for role in SOURCE_ROLES
                   if record['source_sha256'] == fingerprints[role]['sha256']]
        _require(bool(matches), 'Record is from another capture')
        if kind == 'benchmark':
            stamp = record.get('build')
            _require('gfx' in matches and (_hex(stamp, 8) or _hex(stamp, 40)) and build.startswith(stamp),
                     'Benchmark record build or graphics source differs')
        if kind == 'presentation':
            _require('gfx' in matches, 'Submission record is from the wrong source')
        if kind.startswith('runtime_'):
            _require('openxr' in matches, 'Runtime record is from the wrong source')
        if kind in ADMISSION_OBSERVATIONS:
            _require(ADMISSION_OBSERVATIONS[kind][0] in matches,
                     'FG admission record is from the wrong source')
        line = record['line_number']
        _require(_uint(line) and line > 0 and any(
            line <= len(source_lines[role])
            and _digest(source_lines[role][line - 1].encode()) == record['source_line_sha256']
            for role in matches), 'Record line differs from capture source')
    return fingerprints


def _verify_admission_observations(qualification, records, fingerprints, start, end):
    """Resolve each embedded FG observation to a settled, source-verified record."""
    details = qualification.get('treatments', {}).get('fg', {}).get('admission_details')
    if details is None:
        return
    _require(type(details) is dict and type(details.get('schema')) is int and details['schema'] == 1
             and details.get('observation_scope') == 'sparse_diagnostics_not_failure_rates',
             'Unknown FG admission details schema')
    for kind, (role, namespace, fields) in ADMISSION_OBSERVATIONS.items():
        if kind not in details:
            continue
        group = details[kind]
        _require(type(group) is dict and group.get('namespace') == namespace,
                 'Unknown FG admission observation namespace')
        observations = group.get('observations')
        _require(type(observations) is list and bool(observations), 'FG admission observations required')
        settled = {}
        for record in records:
            at = record.get('estimated_at_ms')
            if (record['kind'] == kind and type(record.get('admission_schema')) is int
                    and record['admission_schema'] == 1 and record.get('row_timestamp_qualified') is True
                    and _uint(at) and start <= at <= end):
                settled.setdefault((record['source_line_sha256'], at), []).append(record)
        source_hash = fingerprints[role]['sha256']
        for observation in observations:
            _require(type(observation) is dict
                     and set(observation) == {*fields, 'at_ms', 'source_sha256', 'source_line_sha256'},
                     'Invalid FG admission observation fields')
            _require(observation['source_sha256'] == source_hash
                     and _hex(observation['source_line_sha256'], 64),
                     'FG admission observation source differs')
            at = observation.get('at_ms')
            _require(_uint(at) and start <= at <= end, 'FG admission observation is outside the settled row')
            candidates = settled.get((observation['source_line_sha256'], at), ())
            _require(all(type(observation.get(field)) is int for field in fields) and any(
                record['source_sha256'] == source_hash
                and all(type(record.get(field)) is int and observation[field] == record[field]
                        for field in fields)
                for record in candidates), 'FG admission observation has no matching settled record')


def produce_qualification(run, row, records, receipt, sources, *, aggregate_records, qualify_report):
    """Compute and bind qualification to verified acquisition inputs.

    The trusted adapter supplies aggregate_records(records, run), which returns
    (summary, intervals), and qualify_report(report, row). Reaggregation must
    preserve the supplied normalized records, including parser annotations.
    Callbacks operate on copies; this function neither mutates caller inputs
    nor authenticates the producer. It accepts no cached aggregate or detached result.
    """
    fingerprints = _verify_capture(run, receipt, records, sources)
    inputs = dict(run=run, row=row, records=records)
    snapshot = _canonical_json(inputs)
    copied = deepcopy(inputs)
    report = dict(schema=1, run=copied['run'], records=copied['records'])

    def require_unchanged_inputs():
        _require(_canonical_json(dict(run=report['run'], row=copied['row'], records=report['records'])) == snapshot,
                 'Qualification callbacks changed the normalized inputs')

    summary, intervals = aggregate_records(report['records'], report['run'])
    require_unchanged_inputs()
    _require(type(summary) is dict and type(intervals) is list, 'Invalid recomputed benchmark aggregate')
    summary = summary | {'source_hashes': sorted({source['sha256'] for source in fingerprints.values()})}
    report.update(aggregate=summary, submission_intervals=intervals)
    qualification = qualify_report(report, copied['row'])
    require_unchanged_inputs()
    _require(type(qualification) is dict, 'Invalid reviewed qualification')
    _verify_admission_observations(qualification, records, fingerprints, *_bounds(run))
    binding = _qualification_binding(run, row, records, qualification, receipt, fingerprints)
    return dict(schema=QUALIFICATION_SCHEMA, qualification=deepcopy(qualification), binding=binding)


def _verify_qualification(envelope, run, row, records, receipt, fingerprints):
    _require(type(envelope) is dict and set(envelope) == {'schema', 'qualification', 'binding'}
             and type(envelope['schema']) is int and envelope['schema'] == QUALIFICATION_SCHEMA
             and type(envelope['qualification']) is dict and _hex(envelope['binding'], 64),
             'Bound qualification schema 2 required')
    qualification = envelope['qualification']
    _require(envelope['binding'] == _qualification_binding(run, row, records, qualification, receipt, fingerprints),
             'Qualification binding differs from capture')
    _verify_admission_observations(qualification, records, fingerprints, *_bounds(run))
    return qualification


def submission_summary(records, start, end):
    """Use fresh same-session receiver deltas; report each counter's observed span.

    An unknown endpoint cannot bridge a gap. A regression in any available
    cumulative counter invalidates the interval. Total uses the shared span
    where all three reported submission components are known.
    """
    packets = sorted((item for item in records if item['kind'] == 'presentation'),
                     key=lambda item: item['now_ms'])
    counters = (*SUBMISSION_COUNTERS, 'total')
    elapsed = dict.fromkeys(counters, 0)
    counts = dict.fromkeys(counters, 0)
    intervals = dict.fromkeys(counters, 0)
    previous = None
    observed_ms = observed_intervals = 0
    last_end = start
    for packet in packets:
        now, captured = packet['now_ms'], packet['captured_at_ms']
        _require(all(_uint(value) for value in (now, captured, packet['sequence'])),
                 'Invalid submission timestamp or sequence')
        if not (start <= captured <= now <= end and now - captured <= 2000
                and all(_flag(packet.get(key)) for key in ('active', 'available', 'fresh'))):
            previous = None
            continue
        prior, previous = previous, packet
        if not prior or packet['session_key'] != prior['session_key']:
            continue
        span = captured - prior['captured_at_ms']
        if span <= 0 or prior['captured_at_ms'] < last_end or packet['sequence'] <= prior['sequence']:
            continue
        deltas = {}
        for counter in CUMULATIVE_COUNTERS:
            old, new = prior.get(counter), packet.get(counter)
            _require(all(value is None or _uint(value) for value in (old, new)),
                     'Invalid submission counter')
            if old is not None and new is not None:
                deltas[counter] = new - old
        if any(delta < 0 for delta in deltas.values()):
            continue
        deltas = {counter: deltas[counter] for counter in SUBMISSION_COUNTERS if counter in deltas}
        if sum(deltas.values()) > packet['sequence'] - prior['sequence']:
            continue
        if len(deltas) == len(SUBMISSION_COUNTERS):
            deltas['total'] = sum(deltas.values())
        last_end = captured
        observed_ms += span
        observed_intervals += 1
        for counter, delta in deltas.items():
            elapsed[counter] += span
            counts[counter] += delta
            intervals[counter] += 1
    return {
        'delivery': 'app_xrEndFrame_not_scanout',
        'rates_per_second': {key: counts[key] * 1000 / elapsed[key] if elapsed[key] else None
                             for key in counters},
        'counts': {key: counts[key] if elapsed[key] else None for key in counters},
        'known_counter_coverage': {
            key: {'seconds': elapsed[key] / 1000, 'intervals': intervals[key],
                  'fraction_of_observed': elapsed[key] / observed_ms if observed_ms else None,
                  'fraction_of_row': elapsed[key] / (end - start)}
            for key in counters
        },
        'observed_seconds': observed_ms / 1000,
        'observed_intervals': observed_intervals,
    }


def _clean_window(record):
    """Apply the reviewed clean 30-second CPU/GPU admission rules."""
    return (record.get('eligible') is True and record.get('status') == 'completed'
            and record['end_ms'] - record['start_ms'] == record.get('sample_ms') == 30000
            and record['finished_ms'] - record['end_ms'] == record.get('drain_ms')
            and all(_uint(record.get(series + '_valid')) and record[series + '_valid'] > 0
                    and record[series + '_invalid'] == record[series + '_missing'] == 0
                    and record.get(series) for series in ('cpu', 'gpu')))


def row_diagnostics(records, start, end):
    """Classify observed/rejected windows including their drain; leave crossings whole."""
    contained, crossing, candidates = [], [], {}
    for record in records:
        if record['kind'] != 'benchmark':
            continue
        begin, stop, finished = (record[key] for key in ('start_ms', 'end_ms', 'finished_ms'))
        if not (all(_uint(value) for value in (begin, stop, finished)) and begin <= stop <= finished):
            continue
        if start <= begin and finished <= end:
            contained.append(record)
            if _clean_window(record):
                candidates.setdefault((begin, stop), []).append(record)
        elif begin < end and finished > start:
            crossing.append(record)
    admitted = []
    payload = ('scope', 'cpu', 'gpu', 'cpu_valid', 'gpu_valid', 'input_dimensions',
               'output_dimensions', 'aa', 'model')
    for (begin, _), copies in sorted(candidates.items()):
        first = copies[0]
        if (not admitted or begin >= admitted[-1]['end_ms']) and all(
                all(copy[key] == first[key] for key in payload) for copy in copies[1:]):
            admitted.append(first)
    rejected = [record for record in contained if not _clean_window(record)]
    diagnostics = {
        'observed_benchmark_windows': len(contained),
        'rejected_benchmark_windows': len(contained) - len(admitted),
        'boundary_crossing_benchmark_windows': len(crossing),
    }
    for series in ('cpu', 'gpu'):
        for field in ('invalid', 'missing'):
            key = series + '_' + field
            _require(all(_uint(record[key]) for record in rejected + crossing), 'Invalid sample count')
            diagnostics['rejected_' + key + '_samples'] = sum(record[key] for record in rejected)
            diagnostics['boundary_crossing_' + key + '_samples'] = sum(record[key] for record in crossing)
    return diagnostics, admitted


def _feature_activity(qualification, requested, windows, row, submissions):
    """Copy the reviewed qualifier's allowlisted evidence, then apply stricter checks."""
    treatments = qualification.get('treatments', {})
    result = {}
    for feature in REQUESTS:
        evidence = deepcopy(treatments.get(feature, {}))
        value = evidence.get('status', 'unavailable')
        _require(value in FEATURE_STATUSES, 'Unknown feature status')
        evidence['status'] = value
        result[feature] = evidence
    output = row.get('expected_output_dimensions')
    declared_input = row.get('expected_input_dimensions')
    _require(_dimensions(output), 'Declared stereo output dimensions required')
    _require(declared_input is None or _dimensions(declared_input), 'Invalid declared stereo input dimensions')
    if requested['aa'] == 'native':
        _require(declared_input is None or tuple(declared_input) == tuple(output),
                 'Native input target differs from output target')
        declared_input = output
    dimension_match = bool(windows) and all(
        _dimensions(window['input_dimensions']) and _dimensions(window['output_dimensions'])
        and tuple(window['output_dimensions']) == tuple(output)
        and (declared_input is None or tuple(window['input_dimensions']) == tuple(declared_input))
        and window['aa'] == ('off' if requested['aa'] == 'native' else 'dlss')
        for window in windows)
    result['aa']['benchmark_dimensions_match'] = dimension_match if windows else None
    result['aa']['expected_output_dimensions'] = list(output)
    result['aa']['expected_input_dimensions'] = list(declared_input) if declared_input else None
    if not dimension_match:
        result['aa']['dimensions_verified'] = False
    if windows and not dimension_match:
        result['aa']['status'] = 'wrong_mode'
    elif not windows:
        result['aa']['status'] = 'unavailable'
    generated = submissions['counts']['generated']
    coverage = submissions['known_counter_coverage']['generated']['fraction_of_observed']
    result['fg']['generated_observed'] = (
        True if generated else False if generated == 0 and coverage == 1 else None)
    result['fg']['generated_count'] = generated
    result['fg']['activation_observed'] = result['fg']['generated_observed']
    status = result['fg']['status']
    if status not in ('unsupported', 'inconsistent', 'wrong_mode'):
        if generated is None:
            result['fg']['status'] = 'unavailable'
        elif generated > 0 and (requested['fg'] == 'off' or requested['aa'] == 'native'):
            result['fg']['status'] = 'inconsistent'
        elif status == 'active' and generated == 0 and coverage == 1:
            result['fg']['status'] = 'inconsistent'
        elif (not generated or status != 'active') and coverage < 1:
            result['fg']['status'] = 'partial'
    return result


def _runtime_input_metadata(records):
    """Describe metadata on timing windows already admitted by the reviewed parser."""
    known = unavailable = 0
    for record in records:
        if record['kind'] != 'runtime_cycle' or record.get('aggregate_admitted') is not True:
            continue
        if record['input_dimensions'] is None:
            _require(record.get('input_status') == 'unavailable' and list(record['raw_input_dimensions']) == [0]*4,
                     'Runtime input metadata lacks unavailable provenance')
            unavailable += 1
        else:
            _require(_dimensions(record['input_dimensions']), 'Invalid known runtime input dimensions')
            known += 1
    return {'known_windows': known, 'unavailable_windows': unavailable}


def qualify_capture(run, row, records, qualification, receipt, sources, *, collection_status):
    """Return a portable row result without mutating the inputs or writing files.

    The receipt has before/after process identities, an owned session ID and
    a hash/length for each exact parser input. The caller must obtain those
    identities from live checks bracketing source acquisition. Qualification
    must be the envelope created by produce_qualification during acquisition.
    """
    _require(collection_status in COLLECTION_STATUSES, 'Unknown collection outcome')
    start, end = _bounds(run)
    build = run['expected_build']
    fingerprints = _verify_capture(run, receipt, records, sources)
    qualification = _verify_qualification(qualification, run, row, records, receipt, fingerprints)
    requested = dict(aa=row['nominal_mode'], vrs=row['vrs'], fg=row['frame_generation'])
    _require(all(requested[name] in allowed for name, allowed in REQUESTS.items())
             and (run['aa'], run['vrs'], run['fg']) == (
                 requested['aa'], requested['vrs'] == 'on', requested['fg'])
             and type(run['vrs']) is bool and row['ini_sha256'] == run['config_sha256'],
             'Manifest and capture treatments differ')
    _require(qualification.get('requested') == requested and qualification.get('physical_scanout_qualified') is False,
             'Reviewed qualification differs from capture')
    target = run['target_windows']
    _require(_uint(target) and target > 0, 'Invalid timing window target')
    diagnostics, windows = row_diagnostics(records, start, end)
    submissions = submission_summary(records, start, end)
    supported = row.get('supported') is True and row.get('apply_allowed') is not False and requested['fg'] != '3x'
    if supported:
        features = _feature_activity(qualification, requested, windows, row, submissions)
    else:
        features = {feature: {'status': 'unavailable', 'reason': 'row_not_applied'} for feature in REQUESTS}
        if requested['fg'] == '3x':
            features['fg'] = {'status': 'unsupported', 'reason': 'fg_3x_not_implemented'}
    return {
        'schema': 1, 'build': build, 'source_fingerprints': fingerprints, 'requested': requested,
        'collection_status': collection_status if supported else 'unsupported',
        'timing': {
            'status': ('unsupported' if not supported else 'complete' if len(windows) >= target
                       else 'partial' if windows else 'unavailable'),
            'clean_windows': len(windows), 'target_windows': target,
        },
        'runtime_input_metadata': _runtime_input_metadata(records),
        'submission': submissions, 'row_diagnostics': diagnostics, 'features': features,
        'physical_scanout_qualified': False,
    }


def main(argv=None):
    """Read a capture and print it, or exclusively create a new result file."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path)
    parser.add_argument('--gfx', type=Path)
    parser.add_argument('--openxr', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args(argv)
    if args.self_test:
        if args.dry_run:
            parser.error('--dry-run applies to a capture, not the self-test')
        from edvr_benchmark_test import self_test
        return self_test()
    if not all((args.input, args.gfx, args.openxr)):
        parser.error('--input, --gfx and --openxr are required')
    try:
        capture = json.loads(args.input.read_text(encoding='utf-8-sig'))
        sources = {'gfx': args.gfx.read_bytes(), 'openxr': args.openxr.read_bytes()}
        result = qualify_capture(**capture, sources=sources)
        payload = json.dumps(result, indent=2, allow_nan=False) + '\n'
        if args.output and not args.dry_run:
            with args.output.open('x', encoding='utf-8') as stream:
                stream.write(payload)
        else:
            print(payload, end='')
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
        print(f'Benchmark qualification refused: {error}', file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
