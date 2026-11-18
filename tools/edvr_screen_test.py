"""Synthetic desktop captures for the calibrated screen guard."""

import base64
from copy import deepcopy
from contextlib import redirect_stdout
from io import StringIO
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

from edvr_screen import PNG_SIGNATURE, main, match_screen, read_desktop_png, roi_features


NEUTRAL = [1, 0, 0, 0, 0, 1, 0, 1.6, 0, 0, 1, 0]
BOX = [490, 490, 530, 530]


def png(width, height, *, orange=True):
    """Draw synthetic colored bars split at the center of a black capture."""
    black = b"\0\0\0\xff"
    ink = b"\xff\x88\x10\xff" if orange else b"\xff\xff\xff\xff"
    first = width // 2 - 20
    row = b"\0" + black * first + ink * 40 + black * (width - first - 40)
    raw = (b"\0" + black * width) * (height // 2 - 20) + row * 40
    raw += (b"\0" + black * width) * (height - height // 2 - 20)

    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))

    return (PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def fixture(source=(1280, 720), output=(1024, 576)):
    data = png(*output)
    reference = roi_features(read_desktop_png(data), BOX)
    roi = dict(name="synthetic_label", box=BOX, minimum_iou=.90,
               template_zlib_base64=base64.b64encode(zlib.compress(reference)).decode("ascii"))
    state = dict(capture_edge=max(output), rois=[roi])
    profile = dict(schema=2, profile_id="synthetic-profile", screen_checks=dict(
        schema=1, source_width=source[0], source_height=source[1], allowed_output_edges=[max(output)],
        neutral_hmd_matrix=NEUTRAL.copy(), states={"expected": state}))
    context = dict(source_width=source[0], source_height=source[1], width=output[0], height=output[1],
                   profile_id=profile["profile_id"], hmd_serial="modlinker_sim_head",
                   pose_valid=True, pose_matrix=NEUTRAL.copy())
    return data, profile, context


class ScreenGuardTests(unittest.TestCase):
    def test_landscape_and_square_profile_geometries(self):
        for source, output in (((1280, 720), (1024, 576)), ((2032, 2032), (1024, 1024)),
                               ((2370, 1333), (1024, 576)), ((720, 1280), (576, 1024))):
            with self.subTest(source=source):
                self.assertTrue(match_screen(*fixture(source, output), expected_state="expected")["matched"])

    def test_wrong_source_and_wrong_output_do_not_match(self):
        data, profile, context = fixture()
        altered = dict(context, source_width=1279)
        self.assertFalse(match_screen(data, profile, altered)["matched"])
        altered = dict(context, height=575)
        self.assertIn("differs from PNG", match_screen(data, profile, altered)["reason"])
        self.assertIn("does not scale proportionally", match_screen(png(1024, 575), profile, altered)["reason"])
        profile["screen_checks"]["allowed_output_edges"] = [1280]
        self.assertFalse(match_screen(data, profile, context)["matched"])

    def test_weak_threshold_or_wrong_pixels_do_not_match(self):
        data, profile, context = fixture()
        wrong_color = match_screen(png(1024, 576, orange=False), profile, context)
        self.assertFalse(wrong_color["matched"])
        self.assertLess(wrong_color["metrics"]["expected"]["variants"][0]["rois"]["synthetic_label"]["iou"], .90)
        profile["screen_checks"]["states"]["expected"]["rois"][0]["minimum_iou"] = .899999
        self.assertIn("minimum IoU", match_screen(data, profile, context)["reason"])

    def test_pose_serial_profile_and_ambiguous_observation_refuse(self):
        data, profile, context = fixture()
        for changes in (dict(pose_matrix=[0]*12), dict(hmd_serial="wrong-head"),
                        dict(profile_id="wrong-profile"), dict(pose_valid=False),
                        dict(status=dict(poses=dict(devices=[])))):
            with self.subTest(changes=changes):
                self.assertFalse(match_screen(data, profile, dict(context, **changes))["matched"])
        context["status"] = {"poses": {"devices": [
            {"device": {"index": 0, "serial": context["hmd_serial"]}, "valid": True, "connected": True,
             "tracking_result": 200, "matrix_3x4_row_major": context["pose_matrix"]}]}}
        self.assertTrue(match_screen(data, profile, context)["matched"])
        context["status"]["poses"]["devices"].append(context["status"]["poses"]["devices"][0])
        self.assertFalse(match_screen(data, profile, context)["matched"])

    def test_ambiguous_and_unexpected_state_refuse(self):
        data, profile, context = fixture()
        self.assertFalse(match_screen(data, profile, context, expected_state="elsewhere")["matched"])
        states = profile["screen_checks"]["states"]
        states["ambiguous"] = deepcopy(states["expected"])
        self.assertEqual(match_screen(data, profile, context, expected_state="expected")["reason"], "ambiguous screen")

    def test_corrupt_png_refuses(self):
        data, profile, context = fixture()
        for malformed in (data[:-1], data[:-1] + bytes([data[-1] ^ 1]), data + b"extra"):
            self.assertFalse(match_screen(malformed, profile, context)["matched"])

    def test_cli_is_read_only(self):
        data, profile, context = fixture()
        with tempfile.TemporaryDirectory() as scratch, redirect_stdout(StringIO()) as output:
            root = Path(scratch)
            (root / "capture").write_bytes(data)
            for name, value in (("profile", profile), ("context", context)):
                (root / name).write_text(json.dumps(value), encoding="utf-8")
            argv = [arg for name in ("capture", "profile", "context") for arg in ("--" + name, str(root / name))]
            before = {path.name: path.read_bytes() for path in root.iterdir()}
            self.assertEqual(main(argv + ["--expected-state", "expected"]), 0)
            self.assertTrue(json.loads(output.getvalue())["matched"])
            self.assertEqual(main(argv + ["--expected-state", "wrong"]), 1)
            self.assertEqual(before, {path.name: path.read_bytes() for path in root.iterdir()})


def self_test():
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ScreenGuardTests)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
