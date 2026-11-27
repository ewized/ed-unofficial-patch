"""Fetch NVIDIA's DLSS SDK -- only what the build uses -- pinned and verified.

    python tools/fetch_ngx.py [--into DIR] [--dry-run]
    python tools/fetch_ngx.py --verify DIR
    python tools/fetch_ngx.py --self-test

A sparse, shallow clone of https://github.com/NVIDIA/DLSS at ONE pinned
commit (the 310.9.1 SDK), taking the headers, the x64 static library, the
release runtime and NVIDIA's licence: about 200 MB on disk, most of it the
runtimes. The compiled headers, static library, runtime and licence are
verified against that commit after fetching and before compiling. Update the
commit and the file pins together when qualifying another SDK.

Where it goes: %LOCALAPPDATA%\\EDUP\\ngx-sdk by default -- one copy per machine,
which every checkout and worktree finds (build.bat looks at EDUP_NGX_SDK,
then the checkout's own third_party\\ngx, then here, then the folder from before
the rename, %LOCALAPPDATA%\\EDVR\\ngx-sdk) -- or --into somewhere else,
third_party\\ngx for a checkout that must be self-contained. The SDK's
licence keeps it out of this tree: it may ship inside an application (the
installer carries the runtime) and not on its own, and not under an open
source licence, which a copy in this repository would be.

--verify DIR checks an existing copy and exits non-zero on a missing or
changed file. A checkout or an extracted SDK can supply the same inputs.

--dry-run prints the git commands a fetch would run and runs none of them: no
directory, no clone, no network. --self-test checks the verification and the
fetch's command sequence against fixtures in the temp folder with subprocess
and the network replaced for the whole run, so it never fetches anything;
build.bat runs it before it trusts --verify with the build.
"""
import argparse
import hashlib
import os
import subprocess
import sys

sys.dont_write_bytecode = True    # a dry run must not write an import cache for the helper
import sdk_paths  # noqa: E402

URL = "https://github.com/NVIDIA/DLSS"
# The commit and its SHA-256 file pins below describe one SDK.
COMMIT = "374959484e79a640feaba44c93ac8cfb0a03f5b5"   # "DLSS 310.9.1 SDK", 2026-09-21
DLL_SHA256 = "3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983"
DLL_VERSION = "310.9.1"
PATHS = ["include", "lib/Windows_x86_64/x64", "lib/Windows_x86_64/rel"]

LIB = os.path.join("lib", "Windows_x86_64", "x64", "nvsdk_ngx_s.lib")
DLL = os.path.join("lib", "Windows_x86_64", "rel", "nvngx_dlss.dll")
LICENCE = "LICENSE.txt"
# The include closure of dlaa.cpp and frame_generation_ngx.{h,cpp} for the
# linked Windows build. nvsdk_ngx_helpers.h also includes the CUDA helper.
HEADER_SHA256 = {
    "nvsdk_ngx.h": "dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848",
    "nvsdk_ngx_defs.h": "ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e",
    "nvsdk_ngx_params.h": "943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc",
    "nvsdk_ngx_helpers.h": "5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471",
    "nvsdk_ngx_helpers_cuda.h": "f2851a76107bdf4fbcb7b261f3c35573c312d8ecb4bc18fb02d9e2cec7705042",
    "nvsdk_ngx_helpers_d3d.h": "ec75224f36ed6580baaf250fa83620405ad81a1d98c2c436654a0c9cf6a6b8ba",
    "nvsdk_ngx_helpers_dlssg_d3d.h": "6335ebcb95de39a361e87fe0257d6ce0723e5d8deaa15db3b35afacf2b3f4220",
    "nvsdk_ngx_params_dlssg.h": "c24fa5aa68ffa2808c828b75cfa32843f1606f80a145f84055ac450d7e6c74ad",
    "nvsdk_ngx_defs_dlssg.h": "98ec3a6faa5aa250828c174bf8f3491aafac693dfe57cc798049c8918597fc60",
}
FILES = {os.path.join("include", name): digest for name, digest in HEADER_SHA256.items()}
FILES.update({
    LIB: "4e5d355086d2bc11e1a0842457d2519ea528ee1f3e112c45679a84960c07dff3",
    DLL: DLL_SHA256,
    LICENCE: "d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0",
})


def default_dest():
    return sdk_paths.default_dest("ngx-sdk")


def run(*args, dry_run=False):
    print("(dry run) +" if dry_run else "+", " ".join(args))
    if not dry_run:
        subprocess.check_call(args)


def head_of(dest):
    try:
        out = subprocess.check_output(["git", "-C", dest, "rev-parse", "HEAD"],
                                      stderr=subprocess.DEVNULL)
        return out.decode().strip()
    except (subprocess.CalledProcessError, OSError):
        return ""


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def verify(dest, quiet=False):
    """Verify the exact selected SDK files, including transitive headers."""
    problems = []
    for rel, expected in FILES.items():
        path = os.path.join(dest, rel)
        if not os.path.isfile(path):
            problems.append("missing: %s" % path)
            continue
        if rel in (LIB, DLL):
            got = sha256_of(path)
        else:
            # NVIDIA commits LF text. Git for Windows can check out the same
            # file with CRLF throughout. Accept those two complete encodings.
            with open(path, "rb") as stream:
                data = stream.read()
            if b"\n" not in data.replace(b"\r\n", b""):
                data = data.replace(b"\r\n", b"\n")
            got = hashlib.sha256(data).hexdigest()
        if got != expected:
            problems.append("%s is not the pinned %s SDK file: its SHA-256 is %s, the pin is %s" %
                            (rel, DLL_VERSION, got.upper(), expected.upper()))
    head = head_of(dest)
    if head and head != COMMIT:
        # Not fatal on its own -- the hash above is what matters -- but said.
        if not quiet:
            print("note: the clone at %s is at commit %s, not the pinned %s" % (dest, head[:12], COMMIT[:12]))
    if not problems and not quiet:
        print("DLSS SDK %s verified at %s (nvngx_dlss.dll SHA-256 %s)" % (DLL_VERSION, dest, FILES[DLL].upper()))
    return problems


def fetch(dest, dry_run=False):
    """Clone the pinned SDK into dest and verify it. With dry_run, print the
    commands and run none: the directory is not made and nothing is fetched."""
    if not dry_run:
        os.makedirs(dest, exist_ok=True)
    if not os.path.isdir(os.path.join(dest, ".git")):
        run("git", "init", "-q", dest, dry_run=dry_run)
        run("git", "-C", dest, "remote", "add", "origin", URL, dry_run=dry_run)
    if head_of(dest) != COMMIT:
        # A shallow, blob-less fetch of the one commit; GitHub serves a fetch
        # by full commit id. The sparse checkout then materialises only the
        # paths the build reads (cone mode keeps the top-level files, the
        # licence among them).
        run("git", "-C", dest, "fetch", "--depth", "1", "--filter=blob:none", "origin", COMMIT,
            dry_run=dry_run)
        run("git", "-C", dest, "sparse-checkout", "set", *PATHS, dry_run=dry_run)
        run("git", "-C", dest, "checkout", "-q", "--detach", COMMIT, dry_run=dry_run)
    else:
        run("git", "-C", dest, "sparse-checkout", "set", *PATHS, dry_run=dry_run)
        run("git", "-C", dest, "checkout", "-q", "--detach", COMMIT, dry_run=dry_run)
    if dry_run:
        print("(dry run) nothing was fetched or verified, and nothing was written")
        return
    problems = verify(dest)
    if problems:
        sys.exit("the fetched SDK is not the pinned one:\n  " + "\n  ".join(problems))
    print("DLSS SDK ready at %s -- build.bat finds it there." % dest)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--into", default=None, help="where to put the SDK (default: %%LOCALAPPDATA%%\\EDUP\\ngx-sdk)")
    ap.add_argument("--verify", default=None, metavar="DIR", help="check an existing copy and exit")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the git commands a fetch would run and run none of them")
    ap.add_argument("--self-test", action="store_true",
                    help="check this script against fixtures, never touching the network, and exit")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.verify:
        problems = verify(args.verify)
        if problems:
            for p in problems:
                print("fetch_ngx: " + p)
            return 1
        return 0
    fetch(args.into or default_dest(), dry_run=args.dry_run)
    return 0


# ---------------------------------------------------------------------------
# --self-test
# ---------------------------------------------------------------------------

def self_test():
    """The verification, the destination and the fetch's command sequence,
    against fixtures in the temp folder. Never fetches: subprocess and the
    network are replaced for the whole run, and a call that reaches either
    fails the test instead of going out."""
    import contextlib
    import io
    import shutil
    import socket
    import tempfile

    global FILES
    failures = []
    ran = []                     # every command the code under test tried to run
    touched = []                 # every attempt to reach the network
    git_head = [""]              # what `git rev-parse HEAD` answers (empty: not a repo)
    materialise = [None]         # what a `git checkout` leaves in the directory

    def expect(name, condition, what=""):
        if not condition:
            failures.append("%s: %s" % (name, what))

    def fake_check_call(args, *_a, **_k):
        args = list(args)
        ran.append(args)
        if args[:2] == ["git", "-C"] and args[3] == "checkout" and materialise[0]:
            materialise[0](args[2])
        return 0

    def fake_check_output(args, *_a, **_k):
        ran.append(list(args))
        if not git_head[0]:
            raise subprocess.CalledProcessError(128, args)
        return (git_head[0] + "\n").encode()

    def no_network(*a, **k):
        touched.append(a)
        raise AssertionError("the self-test tried to use the network")

    def capture(fn, *a, **k):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            try:
                result = fn(*a, **k)
            except SystemExit as exc:
                result = "SystemExit: %s" % exc
        return result, out.getvalue()

    def sdk(root, dll=b"the runtime"):
        """A directory that has every file the build reads."""
        for rel in FILES:
            path = os.path.join(root, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(dll if rel == DLL else b"x")
        return root

    base = tempfile.mkdtemp(prefix="edvr-fetch-ngx-")
    saved = (subprocess.check_call, subprocess.check_output, subprocess.Popen,
             socket.create_connection, socket.getaddrinfo, FILES)
    own_connect = "connect" in vars(socket.socket)
    saved_connect = socket.socket.connect
    subprocess.check_call, subprocess.check_output = fake_check_call, fake_check_output
    subprocess.Popen = no_network
    socket.socket.connect = no_network
    socket.create_connection = no_network
    socket.getaddrinfo = no_network
    try:
        # Where it goes when --into does not say.
        sdk_paths.check_fetch_default(default_dest, "ngx-sdk", expect, main)

        # The hash reads a file that is longer than one chunk, exactly.
        name = "sha256"
        big = os.path.join(base, "big.bin")
        data = bytes(range(256)) * (2 * 4096 + 1) + b"tail"       # just over two 1 MiB chunks
        with open(big, "wb") as f:
            f.write(data)
        expect(name, sha256_of(big) == hashlib.sha256(data).hexdigest(), sha256_of(big))

        # verify(): what is missing is named, a runtime that is not the pinned one
        # is named with both hashes, and the pinned one passes quietly or loudly.
        name = "verify-missing"
        empty = os.path.join(base, "empty")
        os.makedirs(empty)
        problems = verify(empty, quiet=True)
        expect(name, len(problems) == len(FILES) and all(p.startswith("missing: ") for p in problems), repr(problems))
        expect(name, any(os.path.join(empty, DLL) in p for p in problems), repr(problems))

        FILES = {rel: hashlib.sha256(b"x").hexdigest() for rel in FILES}
        FILES[DLL] = DLL_SHA256
        name = "verify-wrong-runtime"
        wrong = sdk(os.path.join(base, "wrong"), dll=b"another runtime")
        problems = verify(wrong, quiet=True)
        expect(name, len(problems) == 1 and "is not the pinned %s SDK file" % DLL_VERSION in problems[0], repr(problems))
        expect(name, hashlib.sha256(b"another runtime").hexdigest().upper() in problems[0]
               and DLL_SHA256.upper() in problems[0], repr(problems))

        name = "verify-pinned"
        FILES[DLL] = hashlib.sha256(b"the runtime").hexdigest()      # pin the fixture's runtime
        right = sdk(os.path.join(base, "right"))
        problems, said = capture(verify, right)
        expect(name, problems == [] and "verified at %s" % right in said and FILES[DLL].upper() in said,
               "%r %r" % (problems, said))
        problems, said = capture(verify, right, True)
        expect(name, problems == [] and said == "", "quiet: %r %r" % (problems, said))
        # Keep the SR DLL and location unchanged while replacing each other
        # input, including the library and headers reached through other headers.
        for rel in FILES:
            if rel == DLL:
                continue
            path = os.path.join(right, rel)
            os.remove(path)
            expect("verify-missing-input", verify(right, quiet=True) == ["missing: %s" % path], rel)
            with open(path, "wb") as stream:
                stream.write(b"refreshed SDK file")
            problems = verify(right, quiet=True)
            expect("verify-mixed-sdk", len(problems) == 1 and rel in problems[0], repr(problems))
            with open(path, "wb") as stream:
                stream.write(b"x")
        # A normal Windows checkout changes all text line endings together.
        rel = os.path.join("include", "nvsdk_ngx_defs.h")
        path = os.path.join(right, rel)
        FILES[rel] = hashlib.sha256(b"first\nsecond\n").hexdigest()
        for raw, valid in ((b"first\nsecond\n", True), (b"first\r\nsecond\r\n", True),
                           (b"first\nsecond\r\n", False)):
            with open(path, "wb") as stream:
                stream.write(raw)
            expect("verify-checkout-eol", (not verify(right, quiet=True)) == valid, repr(raw))
        FILES[rel] = hashlib.sha256(b"x").hexdigest()
        with open(path, "wb") as stream:
            stream.write(b"x")
        # A clone at another commit is a note, not a failure: the hash decides.
        git_head[0] = "0123456789abcdef" * 2 + "01234567"
        problems, said = capture(verify, right)
        expect(name, problems == [] and "note: the clone at %s is at commit 0123456789ab" % right in said,
               "%r %r" % (problems, said))
        git_head[0] = ""

        # --verify: the exit code the build reads, and a message per problem.
        name = "verify-cli"
        code, said = capture(main, ["--verify", right])
        expect(name, code == 0, "%r %r" % (code, said))
        code, said = capture(main, ["--verify", wrong])
        expect(name, code == 1 and DLL in said and "is not the pinned" in said, "%r %r" % (code, said))
        code, said = capture(main, ["--verify", empty])
        expect(name, code == 1 and said.count("fetch_ngx: missing: ") == len(FILES), "%r %r" % (code, said))

        # run(): a dry run says the command and does not run it.
        name = "run"
        del ran[:]
        _, said = capture(run, "git", "status")
        expect(name, ran == [["git", "status"]] and said == "+ git status\n", "%r %r" % (ran, said))
        del ran[:]
        _, said = capture(run, "git", "status", dry_run=True)
        expect(name, ran == [] and said == "(dry run) + git status\n", "%r %r" % (ran, said))

        # The fetch's commands, pinned: init and remote for a new directory, the
        # shallow blob-less fetch of the one commit, the sparse paths, the detach.
        fresh = os.path.join(base, "fresh")
        expected = [
            ["git", "init", "-q", fresh],
            ["git", "-C", fresh, "remote", "add", "origin", URL],
            ["git", "-C", fresh, "fetch", "--depth", "1", "--filter=blob:none", "origin", COMMIT],
            ["git", "-C", fresh, "sparse-checkout", "set", *PATHS],
            ["git", "-C", fresh, "checkout", "-q", "--detach", COMMIT],
        ]

        name = "fetch-dry-run"
        del ran[:]
        _, said = capture(fetch, fresh, dry_run=True)
        expect(name, [c for c in ran if "rev-parse" not in c] == [], "a command ran: %r" % ran)
        expect(name, not os.path.exists(fresh), "--dry-run made %s" % fresh)
        for cmd in expected:
            expect(name, "(dry run) + " + " ".join(cmd) in said, "%r not planned in\n%s" % (cmd, said))
        expect(name, "nothing was fetched or verified" in said, said)
        # ... and through the command line.
        del ran[:]
        code, said = capture(main, ["--into", fresh, "--dry-run"])
        expect(name, code == 0 and not os.path.exists(fresh) and
               [c for c in ran if "rev-parse" not in c] == [], "%r %r %r" % (code, said, ran))

        name = "fetch"
        del ran[:]

        def clone(dest):
            sdk(dest)
        materialise[0] = clone
        _, said = capture(fetch, fresh)
        expect(name, [c for c in ran if "rev-parse" not in c] == expected, "ran %r" % ran)
        expect(name, "DLSS SDK ready at %s" % fresh in said, said)

        # A directory that is already a clone of the pin only re-asserts the
        # paths and the checkout: no init, no remote, no fetch.
        name = "fetch-already-cloned"
        again = os.path.join(base, "again")
        os.makedirs(os.path.join(again, ".git"))
        git_head[0] = COMMIT
        del ran[:]
        _, said = capture(fetch, again)
        expect(name, [c for c in ran if "rev-parse" not in c] ==
               [["git", "-C", again, "sparse-checkout", "set", *PATHS],
                ["git", "-C", again, "checkout", "-q", "--detach", COMMIT]], "ran %r" % ran)
        git_head[0] = ""

        # A fetch that lands another runtime fails loudly and says so.
        name = "fetch-wrong-runtime"
        materialise[0] = lambda dest: sdk(dest, dll=b"moved on")
        result, said = capture(fetch, os.path.join(base, "moved"))
        expect(name, str(result).startswith("SystemExit: the fetched SDK is not the pinned one:")
               and "is not the pinned %s SDK file" % DLL_VERSION in str(result), "%r %r" % (result, said))
        materialise[0] = None

        expect("no-network", touched == [], "the network was touched: %r" % (touched,))
        bad = [c for c in ran if c[0] != "git"]
        expect("no-network", bad == [], "a command that is not git ran: %r" % (bad,))
    finally:
        (subprocess.check_call, subprocess.check_output, subprocess.Popen,
         socket.create_connection, socket.getaddrinfo, FILES) = saved
        if own_connect:
            socket.socket.connect = saved_connect
        else:
            del socket.socket.connect
        shutil.rmtree(base, ignore_errors=True)

    if failures:
        print("fetch_ngx: self-test FAILED")
        for f in failures:
            print("  " + f.replace("\n", "\n    "))
        return 1
    print("fetch_ngx: self-test OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
