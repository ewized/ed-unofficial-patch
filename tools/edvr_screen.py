"""Check calibrated desktop text/HUD regions without sending input or writing files."""

import argparse
import base64
import hashlib
import json
import math
from pathlib import Path
import struct
import zlib


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
MAX_PNG_BYTES = 16_000_000
MAX_OUTPUT_EDGE = 1280
MINIMUM_IOU = 0.90
MINIMUM_FOREGROUND = 30


def read_desktop_png(data):
    """Decode only bounded opaque PNG8 RGBA/filter-0 captures; reject all others.

    Returns (width, height, scanlines), retaining each row's filter byte.
    CRC, chunk order, decompressed size, alpha and row filters are checked.
    No graphics or third-party libraries are loaded.
    """
    if not isinstance(data, bytes) or not 57 <= len(data) <= MAX_PNG_BYTES:
        raise ValueError("unsupported PNG byte size")
    if data[:8] != PNG_SIGNATURE:
        raise ValueError("invalid PNG signature")
    offset, chunks = 8, []
    while offset < len(data):
        if offset + 12 > len(data):
            raise ValueError("truncated PNG chunk")
        size = struct.unpack_from(">I", data, offset)[0]
        if size > len(data) - offset - 12:
            raise ValueError("invalid PNG chunk length")
        kind = data[offset + 4:offset + 8]
        payload = data[offset + 8:offset + 8 + size]
        crc = struct.unpack_from(">I", data, offset + 8 + size)[0]
        if zlib.crc32(kind + payload) != crc:
            raise ValueError("PNG CRC mismatch")
        chunks.append((kind, payload))
        offset += size + 12
    if [kind for kind, _ in chunks] != [b"IHDR", b"IDAT", b"IEND"]:
        raise ValueError("unsupported PNG chunks or order")
    if len(chunks[0][1]) != 13 or chunks[2][1]:
        raise ValueError("invalid PNG header or end")
    width, height, depth, color, compression, filtering, interlace = struct.unpack(
        ">IIBBBBB", chunks[0][1])
    if not (1 <= width <= MAX_OUTPUT_EDGE and 1 <= height <= MAX_OUTPUT_EDGE):
        raise ValueError("unsupported PNG capture geometry")
    if (depth, color, compression, filtering, interlace) != (8, 6, 0, 0, 0):
        raise ValueError("only PNG8 RGBA without interlacing is calibrated")
    stride = width * 4 + 1
    expected = height * stride
    decoder = zlib.decompressobj()
    raw = decoder.decompress(chunks[1][1], expected + 1)
    if len(raw) != expected or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
        raise ValueError("invalid bounded PNG image data")
    opaque = b"\xff" * width
    for y in range(height):
        row = y * stride
        if raw[row] != 0:
            raise ValueError("unsupported PNG row filter")
        if raw[row + 4:row + stride:4] != opaque:
            raise ValueError("non-opaque PNG is not calibrated")
    return width, height, raw


def roi_features(image, box, step=2):
    """Sample stable orange/white text in a fixed ROI on a 1024-unit grid.

    A small neighborhood groups antialias colors within one calibrated output
    size. Templates are compared only at that exact capture size. Black and
    scene-background pixels are unlabelled. This does not search or shift a match.
    """
    width, height, raw = image
    left, top, right, bottom = box
    if not (all(type(v) is int for v in box)
            and 0 <= left < right <= 1024 and 0 <= top < bottom <= 1024 and step == 2):
        raise ValueError("unsupported ROI")
    features = bytearray()
    stride = width * 4 + 1
    for y in range(top, bottom, step):
        py = min(height - 1, round((y + 0.5) * height / 1024))
        for x in range(left, right, step):
            px = min(width - 1, round((x + 0.5) * width / 1024))
            orange = white = 0
            for dy, dx in ((0, 0), (0, 1), (1, 0), (1, 1)):
                i = min(height - 1, py + dy) * stride + 1 + min(width - 1, px + dx) * 4
                red, green, blue = raw[i:i + 3]
                orange += red >= 150 and 40 <= green <= 200 and blue <= 85 and red > green * 1.2
                white += min(red, green, blue) >= 185 and max(red, green, blue) - min(red, green, blue) <= 45
            features.append(1 if orange >= 2 else 2 if white >= 2 else 0)
    return bytes(features)


def _pose(context):
    """Read actual HMD observation, never a copied profile pose assertion."""
    if "status" not in context:
        return context.get("pose_matrix"), context.get("hmd_serial"), context.get("pose_valid") is True
    status = context["status"]
    records = [row for row in status.get("poses", {}).get("devices", [])
               if row.get("device", {}).get("index") == 0]
    if len(records) == 1:
        row = records[0]
        return (row.get("matrix_3x4_row_major"), row.get("device", {}).get("serial"),
                row.get("valid") is True and row.get("connected") is True and row.get("tracking_result") == 200)
    raise ValueError("exactly one observed HMD device required")


def _finite_matrix(values):
    return (isinstance(values, list) and len(values) == 12
            and all(type(value) in (int, float) and math.isfinite(value) for value in values))


def validate_context(context, image, profile):
    """Require the configured profile, actual neutral pose and exact capture size."""
    if not isinstance(context, dict):
        raise ValueError("capture context and actual HMD pose required")
    width, height, _ = image
    checks = profile["screen_checks"]
    source = checks["source_width"], checks["source_height"]
    if not all(type(value) is int and 1 <= value <= 16384 for value in source):
        raise ValueError("invalid calibrated source client geometry")
    observed_source = context.get("source_width"), context.get("source_height")
    observed_output = context.get("width"), context.get("height")
    if not all(type(value) is int for value in (*observed_source, *observed_output)):
        raise ValueError("integer capture metadata required")
    if observed_source != source:
        raise ValueError("unsupported source client geometry")
    if observed_output != (width, height):
        raise ValueError("capture metadata differs from PNG")
    edges = checks["allowed_output_edges"]
    if not isinstance(edges, list) or not edges or not all(
            type(edge) is int and 1 <= edge <= MAX_OUTPUT_EDGE for edge in edges):
        raise ValueError("invalid calibrated output edges")
    # Match the capture producer's no-upscale and nearest-pixel rounding rule.
    sizes = {tuple(max(1, round(value * min(1, edge / max(source)))) for value in source)
             for edge in edges}
    if observed_output not in sizes:
        raise ValueError("capture output does not scale proportionally from the calibrated client")
    if context.get("profile_id") != profile["profile_id"]:
        raise ValueError("calibrated profile identity required")
    matrix, serial, valid = _pose(context)
    expected = checks["neutral_hmd_matrix"]
    if not valid or serial != "modlinker_sim_head" or not _finite_matrix(matrix):
        raise ValueError("valid observed virtual HMD pose required")
    if not _finite_matrix(expected):
        raise ValueError("invalid calibrated neutral pose")
    if any(abs(value - target) > 0.00001 for value, target in zip(matrix, expected)):
        raise ValueError("head pose differs from calibrated neutral pose")


def check_screen(png, *, profile_path, expected_state=None, context=None):
    """Return a bounded visual-state decision and all candidate ROI metrics.

    png is encoded bytes or a local path. context supplies source/output sizes,
    profile_id and an actual xr_sim_status under 'status' (or observed flat
    pose_matrix/hmd_serial/pose_valid fields). The owning runner separately
    guards process identity, freshness, focus, input ownership and dashboard.
    Unsupported input, unknown/ambiguous screens, wrong pose and a mismatched
    expected_state return matched=False. No exception authorizes navigation.
    """
    try:
        profile = json.loads(Path(profile_path).read_text(encoding="utf-8"))
        if isinstance(png, bytes):
            data = png
        else:
            with Path(png).open("rb") as stream:
                data = stream.read(MAX_PNG_BYTES + 1)
    except (OSError, ValueError, TypeError) as error:
        return {"matched": False, "state": None, "reason": str(error), "metrics": {}, "profile_id": None}
    return match_screen(data, profile, context, expected_state=expected_state)


def match_screen(data, profile, context, *, expected_state=None):
    """Apply the same matcher to in-memory profile and capture evidence."""
    result = {"matched": False, "state": None, "reason": None, "metrics": {}, "profile_id": None}
    try:
        if (profile.get("schema") != 2 or profile.get("screen_checks", {}).get("schema") != 1
                or not isinstance(profile.get("profile_id"), str) or not profile["profile_id"]):
            raise ValueError("unsupported calibrated profile schema")
        result["profile_id"] = profile["profile_id"]
        result["capture_sha256"] = hashlib.sha256(data).hexdigest()
        image = read_desktop_png(data)
        validate_context(context, image, profile)
        matches = []
        for state, definition in profile["screen_checks"]["states"].items():
            variants = []
            for variant in definition.get("variants", [definition]):
                if variant.get("capture_edge") != max(image[:2]):
                    variants.append({"name": variant.get("name", "recorded_reference"), "rois": {},
                                     "passed": False, "reason": "output size has no calibrated template"})
                    continue
                rois = {}
                for roi in variant["rois"]:
                    name, minimum = roi["name"], roi["minimum_iou"]
                    if not isinstance(name, str) or not name or name in rois:
                        raise ValueError("ROI names must be nonempty and unique within a variant")
                    if type(minimum) not in (int, float) or not MINIMUM_IOU <= minimum <= 1:
                        raise ValueError("calibrated minimum IoU must be between 0.90 and 1")
                    actual = roi_features(image, roi["box"])
                    decoder = zlib.decompressobj()
                    reference = decoder.decompress(base64.b64decode(roi["template_zlib_base64"], validate=True), len(actual) + 1)
                    if len(reference) != len(actual) or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
                        raise ValueError("invalid calibrated template")
                    if any(value > 2 for value in reference) or sum(bool(value) for value in reference) < MINIMUM_FOREGROUND:
                        raise ValueError("calibrated template has invalid or insufficient text features")
                    intersection = sum(a == b and a != 0 for a, b in zip(actual, reference))
                    union = sum(bool(a or b) for a, b in zip(actual, reference))
                    iou = intersection / union if union else 0.0
                    rois[name] = {"iou": round(iou, 6), "intersection": intersection, "union": union,
                                  "minimum_iou": minimum, "passed": iou >= minimum}
                variants.append({"name": variant.get("name", "recorded_reference"), "rois": rois,
                                 "passed": bool(rois) and all(row["passed"] for row in rois.values())})
            passed = any(row["passed"] for row in variants)
            result["metrics"][state] = {"passed": passed, "variants": variants}
            if passed:
                matches.append(state)
        if len(matches) != 1:
            result["reason"] = "unknown screen" if not matches else "ambiguous screen"
            return result
        result["state"] = matches[0]
        if expected_state is not None and expected_state != matches[0]:
            result["reason"] = "recognized screen differs from expected state"
            return result
        result["matched"], result["reason"] = True, "calibrated text/HUD regions matched"
    except (OSError, ValueError, KeyError, TypeError, AttributeError, OverflowError, struct.error, zlib.error) as error:
        result["reason"] = str(error)
    return result


def main(argv=None):
    """Print the guard decision; exit 1 when a capture does not match."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path)
    parser.add_argument("--profile", type=Path)
    parser.add_argument("--context", type=Path)
    parser.add_argument("--expected-state")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    inputs = (args.capture, args.profile, args.context, args.expected_state)
    if args.self_test:
        if any(inputs):
            parser.error("--self-test cannot be combined with capture inputs")
        from edvr_screen_test import self_test
        return self_test()
    if not all(inputs):
        parser.error("--capture, --profile, --context and --expected-state are required")
    try:
        context = json.loads(args.context.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        parser.error(str(error))
    result = check_screen(args.capture, profile_path=args.profile, context=context, expected_state=args.expected_state)
    print(json.dumps(result, indent=2))
    return 0 if result["matched"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
