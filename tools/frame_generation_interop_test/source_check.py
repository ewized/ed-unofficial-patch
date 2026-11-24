"""CPU-only transport source/API guard checks."""
import argparse
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", required=True, action="store_true")
    parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = (root / "src/d3d11/frame_generation_interop.cpp").read_text(encoding="utf-8")
    calls = set(re.findall(r"context4->(\w+)\s*\(", source))
    assert calls == {"CopyResource", "Signal", "Flush"}, calls
    assert not any(word in source for word in ("SetMultithreadProtected", "AcquireSync", "SetEventOnCompletion", "WaitForSingleObject", "CreateCommandQueue", "->Wait(", "SharedTextureTransfer"))
    for method in ("begin", "capture", "publish", "abort"):
        body = source.split(f"FrameGenerationInterop::{method}(", 1)[1]
        assert body.index("impl_->owner(context)") < body.index("impl_->slots")
    initializer = source.split("FrameGenerationInterop::initialize(", 1)[1]
    assert initializer.index("if (!config.enabled)") < initializer.index("new Impl") < initializer.index("LoadLibraryExW")
    assert initializer.index("config.renderThreadId != GetCurrentThreadId()") < initializer.index("new Impl")
    seal = source.split("bool seal(", 1)[1].split("bool image(", 1)[0]
    assert seal.index("context4->Signal") < seal.index("context4->Flush")
    publish = source.split("FrameGenerationInterop::publish(", 1)[1].split("FrameGenerationInterop::abort(", 1)[0]
    assert publish.index("transaction.paired") < publish.index("impl_->seal") < publish.index("published = true")
    assert "D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED" in source
    print("PASS FG interop source guards: context owner, CopyResource/Signal/Flush only, no waits/mode changes, OFF no loader")


if __name__ == "__main__":
    main()
