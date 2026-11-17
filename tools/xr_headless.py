#!/usr/bin/env python3
"""Exercise an already provisioned Mod Linker virtual HMD through its external helper.

Uses stdin/stdout JSON; never installs or starts SteamVR. Output must be a new
folder with an existing parent. --dry-run writes nothing and executes no helper;
--self-test uses offline fixtures. Sequential compositor mirrors are not frame
benchmarks or proof of physical headset behavior. Keep machine setup external.
"""
import argparse
import base64
import copy
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock
import zlib


def require(condition, message):
    if not condition:
        raise ValueError(message)


def absolute_file(value):
    path = Path(value)
    require(path.is_absolute() and path.is_file(), "Expected absolute existing file: " + value)
    return path


def driver_request(op, **fields):
    return {"op": "driver_request", "request": {"op": op, **fields}}


def driver_status(result):
    response = result.get("response", {})
    require(response.get("ok") is True, "Driver rejected request")
    return response["status"]


def head_pose(result):
    """Require actual Running_OK runtime tracking, rather than a driver echo."""
    heads = [p for p in result["devices"] if p["device"].get("device_class") == 1
             and p.get("connected") is True]
    require(len(heads) == 1, "Expected one connected runtime HMD pose")
    head = heads[0]
    require(head["device"]["index"] == 0 and head["device"]["serial"] == "modlinker_sim_head",
            "Runtime HMD is not virtual head at index 0")
    require(head.get("valid") is True and head.get("tracking_result") == 200,
            "Runtime HMD tracking is not valid Running_OK")
    for field, count in (("position", 3), ("orientation", 4)):
        values = head[field]
        require(len(values) == count and all(type(x) in (int, float) and math.isfinite(x) for x in values),
                "Invalid runtime head " + field)
    require(abs(sum(x * x for x in head["orientation"]) - 1) < 0.01,
            "Runtime head orientation is not a unit quaternion")
    return head


def check_runtime(status, poses, expected_pid):
    require(isinstance(status.get("runtime_version"), str) and bool(status["runtime_version"]),
            "Missing runtime version")
    heads = [d for d in status["devices"] if d.get("device_class") == 1 and d.get("connected") is True]
    require(len(heads) == 1 and heads[0]["index"] == 0
            and heads[0]["serial"] == "modlinker_sim_head", "Unexpected connected HMD selection")
    require(status.get("chaperone", {}).get("ready") is True, "Live Chaperone is not ready")
    if expected_pid is not None:
        require(status.get("scene_focus_process") == expected_pid, "Scene process does not match expected PID")
    return head_pose(poses)


def png_info(data):
    """Validate bounded helper RGBA PNGs, including pixels independently of metadata."""
    require(data[:8] == b"\x89PNG\r\n\x1a\n", "Invalid PNG signature")
    offset, compressed, dimensions = 8, bytearray(), None
    while offset < len(data):
        size = struct.unpack_from(">I", data, offset)[0]
        kind, payload = data[offset + 4:offset + 8], data[offset + 8:offset + 8 + size]
        require(len(payload) == size and offset + size + 12 <= len(data), "Truncated PNG")
        crc = struct.unpack_from(">I", data, offset + size + 8)[0]
        require(zlib.crc32(kind + payload) == crc, "PNG CRC mismatch")
        if kind == b"IHDR":
            width, height, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", payload)
            require(0 < width <= 768 and 0 < height <= 768 and
                    (depth, color, compression, filtering, interlace) == (8, 6, 0, 0, 0),
                    "Unexpected capture dimensions or PNG format")
            dimensions = (width, height)
        elif kind == b"IDAT":
            compressed.extend(payload)
        elif kind == b"IEND":
            require(offset + size + 12 == len(data), "Trailing PNG data")
            break
        offset += size + 12
    else:
        raise ValueError("Missing PNG IEND")
    require(dimensions is not None, "Missing PNG dimensions")
    width, height = dimensions
    limit = (width * 4 + 1) * height
    decoder = zlib.decompressobj()
    raw = decoder.decompress(bytes(compressed), limit + 1)
    require(len(raw) == limit and decoder.eof and not decoder.unused_data, "Invalid PNG data size")
    previous, nonzero = bytearray(width * 4), False
    for y in range(height):
        start = y * (width * 4 + 1)
        method, row = raw[start], bytearray(raw[start + 1:start + 1 + width * 4])
        require(method <= 4, "Invalid PNG filter")
        for x in range(len(row)):
            left = row[x - 4] if x >= 4 else 0
            up, corner = previous[x], previous[x - 4] if x >= 4 else 0
            prediction = left + up - corner
            distances = (abs(prediction - left), abs(prediction - up), abs(prediction - corner))
            paeth = (left, up, corner)[distances.index(min(distances))]
            row[x] = (row[x] + (0, left, up, (left + up) // 2, paeth)[method]) & 255
            nonzero = nonzero or (x % 4 != 3 and row[x] != 0)
        previous = row
    require(nonzero, "Uniformly RGB-black capture; scene black or mirror uninitialized")
    return {"width": width, "height": height, "sha256": hashlib.sha256(data).hexdigest()}


def without_images(value):
    """Preserve helper metadata while marking inline pixels stored separately."""
    if isinstance(value, dict):
        return {k: ("stored separately" if k == "png_base64" else without_images(v)) for k, v in value.items()}
    if isinstance(value, list):
        return [without_images(v) for v in value]
    return value


class Client:
    def __init__(self, args, report):
        self.args, self.report = args, report

    def __call__(self, request):
        """Bound each process; record requests and failures before propagating."""
        entry = {"request": request}
        self.report["operations"].append(entry)
        command = [str(self.args.client)]
        if self.args.openvr_dll:
            command.extend(["--openvr-dll", str(self.args.openvr_dll)])
        try:
            process = subprocess.run(command, input=json.dumps(request), capture_output=True,
                                     text=True, encoding="utf-8", timeout=10, check=False)
            entry.update(exit_code=process.returncode, stderr=process.stderr[:4096])
            entry["stdout_sha256"] = hashlib.sha256(process.stdout.encode("utf-8")).hexdigest()
            response = json.loads(process.stdout, parse_constant=lambda value: require(False, "Nonfinite JSON: " + value))
            entry["response"] = without_images(response)
            require(process.returncode == 0 and response.get("ok") is True,
                    "Helper failed: " + str(response.get("error", process.returncode)))
            return response["result"]
        except (Exception, KeyboardInterrupt) as error:
            entry["error"] = str(error) or type(error).__name__
            raise


def exercise(call, args, report, clock=time.monotonic, pause=time.sleep):
    """Move only synthetic head; attempt both cleanup commands after possible start."""
    mutation = False
    try:
        head = check_runtime(call({"op": "status"}), call({"op": "poses", "origin": "standing"}),
                             args.expect_scene_pid)
        state = driver_status(call(driver_request("status")))
        require(state.get("mode") == "virtual_hmd", "Driver is not in virtual_hmd mode")
        require(state.get("active") is False, "Simulator is already active; another caller may own it")
        previous_replays = state.get("replay_completed", 0)
        baseline = head["position"]
        shifted = [baseline[0] + 0.08, baseline[1], baseline[2]]
        mutation = True  # Start may apply even when its client process times out.
        driver_status(call(driver_request("start", head={"position": baseline, "orientation": head["orientation"],
                                                         "tracked": True},
                                          left={"tracked": False}, right={"tracked": False})))
        driver_status(call(driver_request("replay", duration_ms=4000, steps=[
            {"at_ms": 0, "device": "head", "position": shifted},
            {"at_ms": 3000, "device": "head", "position": baseline}])))
        moved, returned, completed = False, False, False
        deadline = clock() + 8
        while clock() < deadline:
            pose = head_pose(call({"op": "poses", "origin": "standing"}))
            moved = moved or math.dist(pose["position"], shifted) < 0.025
            returned = moved and math.dist(pose["position"], baseline) < 0.025
            state = driver_status(call(driver_request("status")))
            completed = state.get("replay") is None and state.get("replay_completed", 0) > previous_replays
            if returned and completed:
                break
            pause(0.1)
        require(moved and returned and completed, "Runtime did not observe head motion, return and replay completion")
        check_runtime(call({"op": "status"}), call({"op": "poses", "origin": "standing"}), args.expect_scene_pid)
        report["head_motion"] = {"observed_translation_and_return": True, "offset_metres": 0.08}
        if args.capture:
            capture = call({"op": "capture", "eye": "both", "max_edge": 768})
            images = capture.get("images", [])
            require(len(images) == 2 and {i.get("eye") for i in images} == {"left", "right"},
                    "Capture must contain both compositor eyes")
            for image in images:
                data = base64.b64decode(image["png_base64"], validate=True)
                name = "compositor-" + image["eye"] + ".png"
                with (args.output / name).open("xb") as stream:
                    stream.write(data)
                evidence = {"eye": image["eye"], "file": name, "sha256": hashlib.sha256(data).hexdigest()}
                report["images"].append(evidence)
                metadata = png_info(data)
                require((image["width"], image["height"]) == (metadata["width"], metadata["height"]),
                        "PNG dimensions disagree with helper metadata")
                evidence.update(metadata)
    except (Exception, KeyboardInterrupt) as error:
        report["errors"].append(str(error) or type(error).__name__)
    finally:
        if mutation:
            for op in ("cancel", "stop"):
                try:
                    state = driver_status(call(driver_request(op)))
                    if op == "stop":
                        require(state.get("active") is False, "Stop did not report inactive simulator")
                except (Exception, KeyboardInterrupt) as error:
                    report["cleanup_errors"].append(op + ": " + (str(error) or type(error).__name__))
    report["ok"] = not report["errors"] and not report["cleanup_errors"]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", type=absolute_file)
    parser.add_argument("--openvr-dll", type=absolute_file)
    parser.add_argument("--source-helper", type=absolute_file,
                        help="Optional original helper copy to hash separately when --client is a proxy")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--expect-scene-pid", type=int)
    parser.add_argument("--capture", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(Tests)
        return 0 if unittest.TextTestRunner().run(suite).wasSuccessful() else 1
    try:
        require(args.client is not None and args.output is not None, "--client and --output are required")
        require(args.expect_scene_pid is None or args.expect_scene_pid > 0, "Expected scene PID must be positive")
        require(not args.output.exists() and not args.output.is_symlink(), "Output path already exists")
        require(args.output.parent.is_dir(), "Output parent must already exist")
        if args.dry_run:
            print("Would verify virtual HMD, replay neutral head motion, cancel/stop, and write " + str(args.output))
            return 0
        source_helper = args.source_helper or args.client
        report = {"ok": False, "client_executable": {"path": str(args.client),
                  "sha256": hashlib.sha256(args.client.read_bytes()).hexdigest()},
                  "source_helper": {"path": str(source_helper),
                  "sha256": hashlib.sha256(source_helper.read_bytes()).hexdigest()},
                  "operations": [], "images": [], "errors": [], "cleanup_errors": [],
                  "scope": "Virtual runtime smoke test; sequential compositor captures; no performance measurement"}
        args.output.mkdir()  # Exclusive claim prevents concurrent output replacement.
        exercise(Client(args, report), args, report)
        with (args.output / "report.json").open("x", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2, allow_nan=False)
            stream.write("\n")
        print(str(args.output / "report.json"))
        return 0 if report["ok"] else 1
    except (OSError, ValueError) as error:
        print("xr_headless: " + str(error), file=sys.stderr)
        return 2


class Tests(unittest.TestCase):
    """Offline protocol fixtures exercise the same safety and cleanup paths as Windows."""
    def fixture(self, failure=None):
        device = {"index": 0, "serial": "modlinker_sim_head", "device_class": 1, "connected": True}
        status = {"runtime_version": "fixture", "devices": [device], "chaperone": {"ready": True}, "scene_focus_process": 42}
        pose = {"device": device, "connected": True, "valid": True, "tracking_result": 200,
                "position": [0, 1.6, 0], "orientation": [0, 0, 0, 1]}
        calls, stage = [], [0]
        def call(request):
            calls.append(request)
            op = request.get("request", request)["op"]
            if failure == op or isinstance(failure, tuple) and op in failure:
                raise TimeoutError("fixture " + op)
            if op == "status" and request["op"] != "driver_request":
                return status
            if op == "poses":
                current = copy.deepcopy(pose)
                if stage[0] == 1:
                    current["position"][0] = 0.08
                    stage[0] = 2
                return {"devices": [current]}
            if op == "replay":
                stage[0] = 1
            return {"response": {"ok": True, "status": {"mode": "virtual_hmd", "active": False,
                    "replay": None, "replay_completed": 2 + int(stage[0] == 2)}}}
        return call, calls, status, pose

    def run_fixture(self, failure=None, capture=False):
        call, calls, _, _ = self.fixture(failure)
        report = {"operations": [], "images": [], "errors": [], "cleanup_errors": []}
        args = argparse.Namespace(expect_scene_pid=42, capture=capture)
        ticks = iter(range(20))
        exercise(call, args, report, clock=lambda: next(ticks), pause=lambda _: None)
        return report, [r.get("request", r)["op"] for r in calls]

    def test_runtime_refusals(self):
        for fault in ("runtime", "chaperone", "serial", "tracking", "second_hmd", "scene"):
            with self.subTest(fault=fault):
                _, _, status, pose = self.fixture()
                if fault == "runtime": status["runtime_version"] = ""
                if fault == "chaperone": status["chaperone"]["ready"] = False
                if fault == "serial": pose["device"]["serial"] = "physical"
                if fault == "tracking": pose["tracking_result"] = 201
                if fault == "second_hmd": status["devices"].append(copy.deepcopy(status["devices"][0]))
                if fault == "scene": status["scene_focus_process"] = 43
                with self.assertRaises(ValueError):
                    check_runtime(status, {"devices": [pose]}, 42)

    def test_replay_and_cleanup(self):
        report, calls = self.run_fixture()
        self.assertTrue(report["ok"], report)
        self.assertEqual(calls[-2:], ["cancel", "stop"])
        for failure in ("start", "replay", "capture", "cancel", "stop", ("replay", "stop")):
            with self.subTest(failure=failure):
                report, calls = self.run_fixture(failure, capture=failure == "capture")
                self.assertFalse(report["ok"])
                self.assertEqual(calls[-2:], ["cancel", "stop"])
                self.assertTrue(report["cleanup_errors"] if failure in ("cancel", "stop") else report["errors"])
                if isinstance(failure, tuple):
                    self.assertTrue(report["errors"] and report["cleanup_errors"])

    def test_main_report_and_client_errors(self):
        with tempfile.TemporaryDirectory() as directory:
            client, original = Path(directory) / "proxy", Path(directory) / "original.exe"
            client.write_bytes(b"proxy")
            original.write_bytes(b"original")
            args = ["--client", str(client), "--source-helper", str(original),
                    "--expect-scene-pid", "42", "--capture"]
            call, calls, _, _ = self.fixture()
            def invoke(command, **kwargs):
                request = json.loads(kwargs["input"])
                if request["op"] == "capture":
                    # A real, dark but nonblack RGBA PNG checks pixel decoding.
                    def chunk(kind, payload):
                        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))
                    data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
                            + chunk(b"IDAT", zlib.compress(b"\x00\x01\x00\x00\xff")) + chunk(b"IEND", b""))
                    result = {"images": [{"eye": eye, "width": 1, "height": 1,
                              "source_width": 1536, "source_height": 1536,
                              "png_base64": base64.b64encode(data).decode("ascii")} for eye in ("left", "right")]}
                else:
                    result = call(request)
                return subprocess.CompletedProcess(command, 0, json.dumps({"ok": True, "result": result}), "")
            output = Path(directory) / "success"
            with mock.patch.object(subprocess, "run", side_effect=invoke):
                self.assertEqual(main(args + ["--output", str(output)]), 0)
            report = json.loads((output / "report.json").read_text(encoding="utf-8"))
            self.assertTrue(report["ok"])
            self.assertEqual(report["source_helper"]["sha256"], hashlib.sha256(b"original").hexdigest())
            self.assertNotEqual(report["source_helper"]["sha256"], report["client_executable"]["sha256"])
            self.assertEqual({p.name for p in output.iterdir()}, {"report.json", "compositor-left.png", "compositor-right.png"})
            self.assertIn('"source_width": 1536', json.dumps(report))
            self.assertNotIn('iVBORw0KGgo', json.dumps(report))
            for index, result in enumerate((subprocess.CompletedProcess([], 1, '{"ok":false,"error":"bad runtime"}', ""),
                                            subprocess.CompletedProcess([], 0, 'invalid JSON', ""),
                                            subprocess.TimeoutExpired("fixture", 10))):
                output = Path(directory) / str(index)
                options = {"side_effect": result} if isinstance(result, Exception) else {"return_value": result}
                with mock.patch.object(subprocess, "run", **options):
                    self.assertEqual(main(args + ["--output", str(output)]), 1)
                report = json.loads((output / "report.json").read_text(encoding="utf-8"))
                self.assertFalse(report["ok"])
                self.assertTrue(report["errors"] and report["operations"][0]["error"])

    def test_stale_replay_counter_fails(self):
        call, calls, _, _ = self.fixture()
        def stale(request):
            result = call(request)
            if request["op"] == "driver_request":
                result["response"]["status"]["replay_completed"] = 2
            return result
        report = {"operations": [], "images": [], "errors": [], "cleanup_errors": []}
        ticks = iter(range(20))
        exercise(stale, argparse.Namespace(expect_scene_pid=42, capture=False), report,
                 clock=lambda: next(ticks), pause=lambda _: None)
        self.assertFalse(report["ok"])
        self.assertIn("replay completion", report["errors"][0])
        self.assertEqual([r["request"]["op"] for r in calls[-2:]], ["cancel", "stop"])

    def test_dry_run_and_collision(self):
        with tempfile.TemporaryDirectory() as directory:
            client, output = Path(directory) / "client.exe", Path(directory) / "result"
            client.write_bytes(b"fixture")
            args = ["--client", str(client), "--output", str(output)]
            with mock.patch.object(subprocess, "run", side_effect=AssertionError("must not execute")):
                self.assertEqual(main(args + ["--dry-run", "--capture"]), 0)
                self.assertEqual(list(Path(directory).iterdir()), [client])
                output.mkdir()
                self.assertEqual(main(args + ["--dry-run"]), 2)

    def test_capture_validation(self):
        def chunk(kind, payload):
            return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))
        header = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
        for pixel in (b"\x00\x00\x00\xff", b"\x01\x00\x00\xff"):
            data = header + chunk(b"IDAT", zlib.compress(b"\x00" + pixel)) + chunk(b"IEND", b"")
            if pixel[0]:
                self.assertEqual(png_info(data)["width"], 1)
            else:
                with self.assertRaises(ValueError): png_info(data)
        self.assertNotIn("secret", json.dumps(without_images({"images": [{"png_base64": "secret"}]})))


if __name__ == "__main__":
    sys.exit(main())
