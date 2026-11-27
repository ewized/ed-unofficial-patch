"""Record and verify the input identity of a green native build.

The full build is allowed to run on a dirty tree so the test suite can check
an uncommitted change.  After that change is committed, the DLL-only
promotion build uses this receipt to prove that the compiled inputs are the
same while allowing the git commit metadata, and therefore ED-UP's displayed
version, to change.

Documentation under docs/ is not a compiled input: the receipt's only
consumer is the DLL-only promotion, which compiles no doc content, so a
doc-only change must not force another full build.

THE INPUT RULE (2026-10-01).  The fingerprint covers exactly:
  1. every file git tracks;
  2. every untracked file git does not ignore (a new source file that has not
     been `git add`ed yet is still an input);
  3. every file under the named ignored dependency folders, DEPENDENCY_ROOTS:
     the fetched NVIDIA SDK, FidelityFX checkout, pinned OpenXR loader, XeSS
     SDK and FSR 4 SDK are git-ignored (licence, size) but ARE compiled or
     linked into the DLLs, so a change to one must change the fingerprint.
Full-to-DLL-only promotion additionally compares the files selected from the
resolved SDK directories in the build context, including shared caches outside
the checkout. Packaging uses only the workspace identity because it compiles
nothing. The build checks the SDK snapshot from before compilation again before
recording a full pass.
Everything else git ignores is not an input: reviews/ (local review write-ups),
logs, build output, scratch.  Writing or updating a local review therefore
cannot make `build.bat --dll-only` refuse when no compiled input changed.  The
tooling and output roots in EXCLUDED_ROOTS (and docs/, above) stay outside the
identity whatever git says about them.  Outside a git work tree there is no
ignore list to ask, so the older rule applies: everything but EXCLUDED_ROOTS.
A receipt written before this rule fingerprinted the ignored files too, so it
does not match a later check: the next promotion needs one full build.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
from datetime import datetime, timezone


SCHEMA = 1
SDK_KEYS = ("ngx", "reflex", "xess", "fsr4", "ffx")
EXCLUDED_ROOTS = {".git", ".claude", ".codex", ".vs", "build", "build - Copy",
                  "dist", "analysis", "edup_logs", "edvr_logs", "docs"}
EXCLUDED_PARTS = {"__pycache__", ".pytest_cache"}
# Git-ignored folders that are compiled inputs and so stay in the fingerprint
# (see THE INPUT RULE above).  Relative to the repository root, forward slashes.
DEPENDENCY_ROOTS = ("third_party/ngx", "third_party/ffx-dx11", "third_party/openxr/loader", "third_party/xess",
                    "third_party/fsr4", "third_party/edhm_2dmigoto", "third_party/reflex")


def _excluded(relative):
    return (relative.parts and relative.parts[0] in EXCLUDED_ROOTS) or \
        any(part in EXCLUDED_PARTS or part.endswith(".pyc") for part in relative.parts)


def git_listed(root):
    """Return the relative paths git lists as inputs (tracked, plus untracked
    files it does not ignore), or None when root is not itself a git work tree
    or git cannot answer: then the caller falls back to the older walk."""
    root = Path(root).resolve()
    try:
        top = subprocess.run(["git", "-C", str(root), "rev-parse", "--show-toplevel"],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        if top.returncode or Path(top.stdout.decode("utf-8", "replace").strip()).resolve() != root:
            return None
        listed = subprocess.run(["git", "-C", str(root), "ls-files", "-z", "--cached", "--others",
                                 "--exclude-standard"],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    except OSError:
        return None
    if listed.returncode:
        return None
    return [name for name in listed.stdout.decode("utf-8", "surrogateescape").split("\0") if name]


def input_files(root):
    """Return every workspace file that can be a build input (THE INPUT RULE
    in the module docstring): git's tracked and untracked-not-ignored files plus
    the ignored dependency folders, minus the excluded roots; everything but
    the excluded roots where root is not a git work tree."""
    root = Path(root).resolve()
    listed = git_listed(root)
    candidates = []
    if listed is None:
        candidates = [path for path in root.rglob("*") if path.is_file()]
    else:
        candidates = [root / name for name in listed]
        for dependency in DEPENDENCY_ROOTS:
            base = root / dependency
            if base.is_dir():
                candidates.extend(path for path in base.rglob("*") if path.is_file())
    files = {}
    for path in candidates:
        if not path.is_file():
            continue   # tracked but deleted: its absence is the change
        relative = path.relative_to(root)
        if _excluded(relative):
            continue
        files[relative.as_posix()] = path
    return sorted(files.items())


def _hash_file_into(digest, path):
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)


def input_fingerprint(root):
    digest = hashlib.sha256()
    for relative, path in input_files(root):
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        try:
            _hash_file_into(digest, path)
        except OSError as error:
            raise ValueError("cannot read build input %s: %s" % (relative, error))
        digest.update(b"\0")
    return digest.hexdigest()


def _sdk_tools():
    """Load the SDK file lists without writing import caches or changing the caller's policy."""
    previous = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        import fetch_ffx_dx11
        import fetch_fsr4
        import fetch_ngx
        import fetch_reflex
        import fetch_xess
        return fetch_ffx_dx11, fetch_fsr4, fetch_ngx, fetch_reflex, fetch_xess
    finally:
        sys.dont_write_bytecode = previous


def sdk_input_hashes(root, context):
    """Hash the fetchers' actual build and staging inputs under each selected SDK root."""
    # Packaging and source-only callers can run without the SDK tools.
    selected = {key: context[key] for key in SDK_KEYS if context.get(key)}
    if selected.get("reflex", "").lower() == "none":
        del selected["reflex"]
    if not selected:
        return {}

    ffx, fetch_fsr4, fetch_ngx, fetch_reflex, fetch_xess = _sdk_tools()
    files = {
        "ngx": fetch_ngx.FILES,
        "reflex": fetch_reflex.FILES,
        "xess": fetch_xess.FILES,
        "fsr4": fetch_fsr4.FILES,
        "ffx": [Path(ffx.HEADER_DEST) / name for name in ffx.HEADERS] +
               [Path("lib") / name for name in ffx.LIBS] + [ffx.LICENCE, ffx.VERSION_FILE],
    }
    hashes = {}
    for sdk, location in selected.items():
        for rel in files[sdk]:
            path = Path(root) / location / rel
            digest = hashlib.sha256()
            _hash_file_into(digest, path)
            hashes[sdk + "/" + Path(rel).as_posix()] = digest.hexdigest()
    return hashes


def sdk_fingerprint(hashes):
    """Summarize the selected files for build.bat to carry across the full gate."""
    data = json.dumps(hashes, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(data).hexdigest()


def command_output(root, *args):
    result = subprocess.run(["git", "-C", str(root), *args],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, check=False)
    if result.returncode:
        return ""
    return result.stdout.strip()


def git_state(root):
    return {
        "commit": command_output(root, "rev-parse", "HEAD") or "unknown",
        "describe": command_output(root, "describe", "--tags", "--always", "--dirty") or "unknown",
    }


def toolchain_state():
    located = subprocess.run(["where", "cl.exe"], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True, check=False)
    path = located.stdout.strip() if located.returncode == 0 else "unknown"
    version = subprocess.run(["cl.exe"], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, check=False)
    lines = [line.strip() for line in version.stdout.splitlines() if line.strip()]
    return {"path": path, "version": lines[0] if lines else "unknown"}


def require_clean(root):
    result = subprocess.run(["git", "-C", str(root), "status", "--porcelain",
                             "--untracked-files=all"],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, check=False)
    if result.returncode:
        raise ValueError("cannot inspect git status")
    if result.stdout.strip():
        raise ValueError("DLL-only promotion requires a clean working tree")


def parse_context(values):
    context = {}
    for value in values:
        key, separator, content = value.partition("=")
        if not separator or not key:
            raise ValueError("context must be KEY=VALUE: %s" % value)
        if key in context:
            raise ValueError("context key repeated: %s" % key)
        context[key] = content
    return dict(sorted(context.items()))


def make_receipt(root, context, sdk_hashes=None):
    if sdk_hashes is None:
        sdk_hashes = sdk_input_hashes(root, context)
    state = git_state(root)
    return {
        "schema": SCHEMA,
        "status": "full-pass",
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "commit": state["commit"],
        "describe": state["describe"],
        "toolchain": toolchain_state(),
        "inputs_sha256": input_fingerprint(root),
        "sdk_inputs_sha256": sdk_hashes,
        "context": context,
    }


def write_receipt(path, root, context, expected_sdk=None, dry_run=False):
    path = Path(path).resolve()
    sdk_hashes = sdk_input_hashes(root, context)
    if expected_sdk is not None and sdk_fingerprint(sdk_hashes) != expected_sdk:
        raise ValueError("resolved SDK inputs changed during the full build")
    if dry_run:
        print("[edup] would write full-build receipt: %s" % path)
        return 0
    receipt = make_receipt(root, context, sdk_hashes)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=str(path.parent),
                                     prefix=path.name + ".", suffix=".tmp",
                                     delete=False) as stream:
        temporary = Path(stream.name)
        json.dump(receipt, stream, indent=2, sort_keys=True)
        stream.write("\n")
    try:
        temporary.replace(path)
    except OSError:
        if temporary.exists():
            temporary.unlink()
        raise
    print("[edup] full-build receipt: %s (%s)" % (path, receipt["inputs_sha256"]))
    return 0


def verify_receipt(path, root, context, clean, tree_only=False, expected_sdk=None):
    """Check a full-pass receipt against the tree.

    tree_only is for packaging, which compiles nothing: it trusts the build's
    outputs when the tree they were built from is unchanged, and does not ask
    for the build context or the C++ toolchain the build ran with.
    """
    path = Path(path).resolve()
    try:
        with path.open(encoding="utf-8") as stream:
            receipt = json.load(stream)
    except (OSError, ValueError) as error:
        raise ValueError("cannot read full-build receipt %s: %s" % (path, error))
    if receipt.get("schema") != SCHEMA or receipt.get("status") != "full-pass":
        raise ValueError("receipt is not a full-pass receipt with schema %d" % SCHEMA)
    if not tree_only:
        if receipt.get("context") != context:
            raise ValueError("build context differs from the last full build")
        recorded = receipt.get("sdk_inputs_sha256")
        if not isinstance(recorded, dict):
            raise ValueError("full-build receipt lacks resolved SDK input hashes; run a full build")
        if expected_sdk is not None and sdk_fingerprint(recorded) != expected_sdk:
            raise ValueError("pre-compilation SDK inputs differ from the last full build")
        actual_sdk = sdk_input_hashes(root, context)
        for name in sorted(recorded.keys() | actual_sdk.keys()):
            if recorded.get(name) != actual_sdk.get(name):
                raise ValueError("resolved SDK input differs from the last full build: %s" % name)
        if receipt.get("toolchain") != toolchain_state():
            raise ValueError("C++ toolchain differs from the last full build")
    actual = input_fingerprint(root)
    if receipt.get("inputs_sha256") != actual:
        raise ValueError("compiled inputs differ from the last full build")
    if clean:
        require_clean(root)
    state = git_state(root)
    if tree_only:
        print("[edup] full-build receipt matches the tree: %s" % path)
    else:
        print("[edup] full-build receipt verified: %s" % path)
    print("[edup] validated inputs from %s; promotion version is %s" %
          (receipt.get("describe", "unknown"), state["describe"]))
    return 0


def _remove_tree(path):
    """Delete a directory tree even where git left read-only object files in it (Windows)."""
    def clear_and_retry(function, target, _error):
        os.chmod(target, stat.S_IWRITE)
        function(target)
    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=clear_and_retry)
    else:
        shutil.rmtree(path, onerror=clear_and_retry)


def _self_test_tree_verify(root):
    """--verify-tree: packaging checks the tree, not the build context, and refuses a changed tree."""
    root.mkdir(parents=True)

    def git(*args):
        result = subprocess.run(["git", "-C", str(root), *args], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, check=False)
        if result.returncode:
            raise AssertionError("git %s failed: %s" % (" ".join(args), result.stderr.decode("utf-8", "replace")))

    git("init", "-q")
    _write(root / ".gitignore", "build/\ndist/\n")
    _write(root / "source.txt", "one")
    git("add", ".gitignore", "source.txt")
    # Unsigned, whatever the user's global config says: a global commit.gpgsign reached this throwaway repo, and gpg's
    # passphrase prompt held the gate for six minutes before the commit failed (2026-11-13). linux_toolchain.py does the same.
    git("-c", "user.email=edvr@example.invalid", "-c", "user.name=edvr", "-c", "commit.gpgsign=false",
        "commit", "-q", "-m", "fixture")
    receipt = root / "build" / "full_build_receipt.json"
    # Written directly: write_receipt would probe the C++ toolchain, which this test does not need.
    receipt.parent.mkdir(parents=True, exist_ok=True)
    with receipt.open("w", encoding="utf-8") as stream:
        json.dump({"schema": SCHEMA, "status": "full-pass", "describe": "fixture",
                   "toolchain": "not probed", "inputs_sha256": input_fingerprint(root),
                   "context": {"cl": "compiler-a"}}, stream)
    # Packaging compiles nothing, so it does not ask for the build context the build ran with.
    if verify_receipt(receipt, root, {"cl": "compiler-b"}, False, tree_only=True) != 0:
        raise AssertionError("a tree-only check refused an unchanged tree")
    # The full check still asks for the context the build ran with.
    try:
        verify_receipt(receipt, root, {"cl": "compiler-b"}, False)
    except ValueError:
        pass
    else:
        raise AssertionError("the full check accepted a different build context")
    # A changed input is refused; restoring it accepts the tree again.
    _write(root / "source.txt", "two")
    try:
        verify_receipt(receipt, root, {}, False, tree_only=True)
    except ValueError:
        pass
    else:
        raise AssertionError("a tree-only check accepted a changed input")
    _write(root / "source.txt", "one")
    verify_receipt(receipt, root, {}, False, tree_only=True)


def _self_test_import_policy(base, sdk):
    """Cold imports, including failed SDK imports, must preserve the caller's bytecode policy."""
    cache = base / "import-cache"
    script = """
import sys
sys.path.insert(0, sys.argv[1])
policy = sys.argv[2] == "True"
sys.dont_write_bytecode = policy
import build_receipt
assert sys.dont_write_bytecode is policy, "importing build_receipt changed the caller's policy"
context = {"reflex": sys.argv[3]}
sys.modules["fetch_xess"] = None
try:
    build_receipt.sdk_input_hashes(".", context)
except ModuleNotFoundError as exc:
    assert exc.name == "fetch_xess"
else:
    raise AssertionError("missing SDK helper accepted")
assert sys.dont_write_bytecode is policy, "failed SDK import changed the caller's policy"
del sys.modules["fetch_xess"]
build_receipt.write_receipt("preview.json", ".", context, dry_run=True)
assert sys.dont_write_bytecode is policy, "receipt preview changed the caller's policy"
"""
    for policy in (False, True):
        subprocess.run([sys.executable, "-I", "-X", "pycache_prefix=" + str(cache), "-c", script,
                        str(Path(__file__).resolve().parent), str(policy), str(sdk)], cwd=base, check=True)
    if (base / "preview.json").exists() or any(cache.rglob("fetch_*.pyc")) or any(cache.rglob("sdk_paths.*.pyc")):
        raise AssertionError("receipt preview created an output or cached an SDK helper")


def _self_test_external_sdk(base):
    """Run the full/verify CLI against a shared SDK that is refreshed in place."""
    from contextlib import redirect_stdout, redirect_stderr
    from io import StringIO
    from unittest.mock import patch
    ffx, fetch_fsr4, fetch_ngx, fetch_reflex, fetch_xess = _sdk_tools()

    root, sdk = base / "checkout", base / "shared SDK"
    root.mkdir(parents=True)
    _write(root / "source.cpp", "source")
    for name in fetch_ngx.FILES:
        _write(sdk / name, ("original " + name).encode("utf-8"))
    receipt = root / "build/full.json"
    context = ["--root", str(root), "--context", "ngx=" + str(sdk), "--context", "reflex=none"]
    snapshot_env = {key.upper(): "" for key in SDK_KEYS}
    snapshot_env.update(NGX=str(sdk), REFLEX="none")

    def cli(args, expected, message):
        output = StringIO()
        with redirect_stdout(output), redirect_stderr(output):
            code = main(args)
        if code != expected or message not in output.getvalue():
            raise AssertionError("receipt CLI %r: %d, %s" % (args, code, output.getvalue()))
        return output.getvalue().strip()

    with patch.dict(os.environ, snapshot_env), patch(__name__ + ".toolchain_state", return_value={"cl": "fixture"}):
        before = cli(["--sdk-fingerprint", "--root", str(root)], 0, "")
        write = ["--write", str(receipt), "--expected-sdk", before] + context
        check = ["--verify", str(receipt)] + context
        cli(write + ["--dry-run"], 0, "would write")
        if receipt.parent.exists():
            raise AssertionError("dry-run created an output directory")
        cli(write, 0, "full-build receipt")
        cli(check + ["--expected-sdk", before], 0, "verified")
        cli(check + ["--expected-sdk", "0" * 64], 1, "pre-compilation SDK inputs differ")
        original_receipt = receipt.read_bytes()
        for name in (Path("include/nvsdk_ngx_defs_dlssg.h"), Path(fetch_ngx.LIB)):
            path = sdk / name
            original = path.read_bytes()
            path.write_bytes(b"replaced")
            cli(check, 1, "ngx/" + name.as_posix())
            cli(["--sdk-fingerprint", "--root", str(root), "--expected-sdk", before], 1, "changed during compilation")
            cli(write, 1, "changed during the full build")
            if receipt.read_bytes() != original_receipt:
                raise AssertionError("a changed SDK replaced the existing receipt")
            path.unlink()
            cli(check, 1, path.name)
            path.write_bytes(original)
            cli(check, 0, "verified")
        # Packaging only checks the workspace and never needs the SDK mount.
        sdk.rename(base / "unmounted")
        cli(["--verify-tree", str(receipt), "--root", str(root)], 0, "matches the tree")
        (base / "unmounted").rename(sdk)
        old = json.loads(original_receipt)
        del old["sdk_inputs_sha256"]
        receipt.write_text(json.dumps(old), encoding="utf-8")
        cli(check, 1, "lacks resolved SDK input hashes")

    for name in (*fetch_reflex.FILES, *fetch_fsr4.FILES, *fetch_xess.FILES, ffx.VERSION_FILE, ffx.LICENCE):
        _write(sdk / name, b"fixture")
    for name in ffx.HEADERS:
        _write(sdk / ffx.HEADER_DEST / name, b"header")
    for name in ffx.LIBS:
        _write(sdk / "lib" / name, b"library")
    selected = {key: str(sdk) for key in ("reflex", "ffx", "fsr4", "xess")}
    before = sdk_input_hashes(root, selected)
    for key, rel in (("reflex", fetch_reflex.HEADER), ("ffx", "lib/" + ffx.LIBS[0]),
                     ("fsr4", fetch_fsr4.DLLS[0]), ("xess", fetch_xess.DLLS[0])):
        _write(sdk / rel, b"updated")
        after = sdk_input_hashes(root, selected)
        if {name for name in before if before[name] != after[name]} != {key + "/" + rel}:
            raise AssertionError("selected %s input did not change its receipt entry" % key)
        before = after
    if sdk_input_hashes(root, {"reflex": "NONE", "ffx": "", "fsr4": "", "xess": ""}):
        raise AssertionError("disabled SDK was included")
    _self_test_import_policy(base, sdk)


def self_test():
    temporary = tempfile.mkdtemp(prefix="edvr-build-receipt-")
    try:
        _self_test_in_git_tree(Path(temporary) / "git")
        _self_test_tree_verify(Path(temporary) / "tree")
        _self_test_without_git(Path(temporary) / "plain")
        _self_test_external_sdk(Path(temporary) / "shared")
    finally:
        _remove_tree(temporary)
    print("build_receipt: self-test passed")
    return 0


def _write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(text, bytes):
        path.write_bytes(text)
    else:
        path.write_text(text, encoding="utf-8")


def _expect_same(root, expected, why):
    if input_fingerprint(root) != expected:
        raise AssertionError(why)


def _expect_changed(root, previous, why):
    if input_fingerprint(root) == previous:
        raise AssertionError(why)


def _self_test_in_git_tree(root):
    """THE INPUT RULE, in a real git work tree with the repository's ignore patterns."""
    root.mkdir(parents=True)

    def git(*args):
        result = subprocess.run(["git", "-C", str(root), *args], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, check=False)
        if result.returncode:
            raise AssertionError("git %s failed: %s" % (" ".join(args), result.stderr.decode("utf-8", "replace")))

    git("init", "-q")
    _write(root / ".gitignore", "build/\ndist/\n__pycache__/\n*.pyc\nreviews/\nscratch.log\n"
           "third_party/ngx/\nthird_party/reflex/\nthird_party/ffx-dx11/\nthird_party/openxr/loader/\nthird_party/xess/\n"
           "third_party/fsr4/\nthird_party/edhm_2dmigoto/\n")
    _write(root / "source.txt", "one")
    _write(root / "tools" / "tool.py", "one")
    git("add", ".gitignore", "source.txt", "tools/tool.py")
    # Not tracked, not ignored: a new source file that has not been added yet is an input.
    _write(root / "src" / "new_file.cpp", "one")
    # Outside the identity: outputs, docs, caches, local reviews and scratch.
    _write(root / "build" / "ignored.txt", "one")
    _write(root / "dist" / "ignored.txt", "one")
    _write(root / "docs" / "notes.md", "one")
    _write(root / "tools" / "__pycache__" / "ignored.pyc", b"one")
    _write(root / "reviews" / "review.md", "one")
    _write(root / "scratch.log", "one")
    # Ignored by git and still compiled inputs.
    _write(root / "third_party" / "ngx" / "lib" / "sdk.lib", "one")
    _write(root / "third_party" / "reflex" / "Reflex_Stats" / "pclstats.h", "one")
    _write(root / "third_party" / "ffx-dx11" / "ffx.h", "one")
    _write(root / "third_party" / "xess" / "include" / "xess" / "xess.h", "one")
    _write(root / "third_party" / "fsr4" / "include" / "FidelityFX" / "upscalers" / "include" / "ffx_upscale.h", "one")
    _write(root / "third_party" / "openxr" / "loader" / "openxr_loader.dll", "one")
    _write(root / "third_party" / "edhm_2dmigoto" / "bin" / "d3d11_edhm.dll", "one")
    first = input_fingerprint(root)

    # Changes that must not matter.
    _write(root / "build" / "ignored.txt", "two")
    _write(root / "docs" / "notes.md", "two")
    _write(root / "tools" / "__pycache__" / "ignored.pyc", b"two")
    _write(root / "reviews" / "review.md", "two")          # a local review updated ...
    _write(root / "reviews" / "second-review.md", "new")   # ... and a new one written
    _write(root / "scratch.log", "two")
    _expect_same(root, first, "build outputs, docs, caches, reviews or other ignored scratch affect the input fingerprint")

    # Changes that must matter, each undone before the next.
    for relative, why in (
            ("third_party/ngx/lib/sdk.lib", "a change under third_party/ngx does not affect the input fingerprint"),
            ("third_party/reflex/Reflex_Stats/pclstats.h", "a change under third_party/reflex does not affect the input fingerprint"),
            ("third_party/ffx-dx11/ffx.h", "a change under third_party/ffx-dx11 does not affect the input fingerprint"),
            ("third_party/xess/include/xess/xess.h", "a change under third_party/xess does not affect the input fingerprint"),
            ("third_party/fsr4/include/FidelityFX/upscalers/include/ffx_upscale.h",
             "a change under third_party/fsr4 does not affect the input fingerprint"),
            ("third_party/openxr/loader/openxr_loader.dll",
             "a change under third_party/openxr/loader does not affect the input fingerprint"),
            ("third_party/edhm_2dmigoto/bin/d3d11_edhm.dll",
             "a change under third_party/edhm_2dmigoto does not affect the input fingerprint"),
            ("source.txt", "a tracked source change does not affect the input fingerprint"),
            ("tools/tool.py", "a tracked tool change does not affect the input fingerprint"),
            ("src/new_file.cpp", "an untracked, not-ignored file's change does not affect the input fingerprint")):
        path = root / relative
        original = path.read_text(encoding="utf-8")
        _write(path, "two")
        _expect_changed(root, first, why)
        _write(path, original)
        _expect_same(root, first, "restoring %s did not restore the input fingerprint" % relative)
    _write(root / "src" / "another_new_file.cpp", "one")
    _expect_changed(root, first, "adding an untracked, not-ignored file does not affect the input fingerprint")
    (root / "src" / "another_new_file.cpp").unlink()
    _write(root / "third_party" / "ngx" / "lib" / "added.lib", "one")
    _expect_changed(root, first, "adding a file under third_party/ngx does not affect the input fingerprint")
    (root / "third_party" / "ngx" / "lib" / "added.lib").unlink()
    (root / "source.txt").unlink()
    _expect_changed(root, first, "deleting a tracked file does not affect the input fingerprint")
    _write(root / "source.txt", "one")
    _expect_same(root, first, "the input fingerprint is not restored once every change is undone")


def _self_test_without_git(root):
    """Outside a git work tree there is no ignore list: everything but the excluded roots counts."""
    (root / "build").mkdir(parents=True)
    (root / "docs").mkdir()
    (root / "tools" / "__pycache__").mkdir(parents=True)
    _write(root / "source.txt", "one")
    _write(root / "build" / "ignored.txt", "one")
    _write(root / "docs" / "notes.md", "one")
    _write(root / "tools" / "__pycache__" / "ignored.pyc", b"one")
    first = input_fingerprint(root)
    _write(root / "build" / "ignored.txt", "two")
    _write(root / "docs" / "notes.md", "two")
    _write(root / "tools" / "__pycache__" / "ignored.pyc", b"two")
    _expect_same(root, first, "build outputs or docs affect the input fingerprint")
    _write(root / "source.txt", "two")
    _expect_changed(root, first, "source changes do not affect the input fingerprint")
    if parse_context(["z=last", "a=first"]) != {"a": "first", "z": "last"}:
        raise AssertionError("context keys are not canonicalized")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--sdk-fingerprint", action="store_true",
                        help="print the SDK digest using the build's resolved NGX, REFLEX, XESS, FSR4 and FFX variables")
    action.add_argument("--write", metavar="PATH", help="write a full-build receipt")
    action.add_argument("--verify", metavar="PATH", help="verify a full-build receipt")
    action.add_argument("--verify-tree", metavar="PATH",
                        help="check a full-build receipt against the tree for packaging, without the build context")
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--context", action="append", default=[], metavar="KEY=VALUE")
    parser.add_argument("--expected-sdk", metavar="SHA256", help="require the SDK digest from before compilation when writing or checking SDK inputs")
    parser.add_argument("--dry-run", action="store_true", help="preview a receipt write without creating files")
    parser.add_argument("--require-clean", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.expected_sdk is not None and not (args.write or args.verify or args.sdk_fingerprint):
        parser.error("--expected-sdk requires --write, --verify or --sdk-fingerprint")
    if not args.write and not args.verify and not args.verify_tree and not args.sdk_fingerprint:
        parser.error("one of --write, --verify, --verify-tree, --sdk-fingerprint or --self-test is required")
    root = args.root.resolve()
    try:
        context = parse_context(args.context)
        if args.sdk_fingerprint:
            selected = {key: os.environ.get(key.upper(), "") for key in SDK_KEYS}
            current = sdk_fingerprint(sdk_input_hashes(root, selected))
            if args.expected_sdk is not None and current != args.expected_sdk:
                raise ValueError("resolved SDK inputs changed during compilation")
            print(current)
            return 0
        if args.write:
            return write_receipt(args.write, root, context, args.expected_sdk, args.dry_run)
        if args.verify_tree:
            return verify_receipt(args.verify_tree, root, context, args.require_clean, tree_only=True)
        return verify_receipt(args.verify, root, context, args.require_clean, expected_sdk=args.expected_sdk)
    except (OSError, ValueError) as error:
        print("build_receipt: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.dont_write_bytecode = True
    sys.exit(main())
