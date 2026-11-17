// Driver-free regression: the production descriptor and ordering protect UI
// whether extension state is replayed by a command list or is inherited.
#include "ui_vrs_guard.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace edvr;
namespace {
int failures = 0;
void check(bool ok, const char* why) { if (!ok) { std::printf("FAIL %s\n", why); ++failures; } }
struct Context { bool coarse = true; int srrv = 73, calls = 0, status = 0; };
int __cdecl reset(IUnknown* raw, UiVrsResetDesc* d) {
    auto& c = *reinterpret_cast<Context*>(raw);
    check(d->version == 0x10010 && d->numViewports == 0 && !d->pViewports, "documented zero-viewport descriptor");
    ++c.calls;
    if (c.status == 0) c.coarse = false;
    return c.status;
}
IUnknown* unknown(Context& c) { return reinterpret_cast<IUnknown*>(&c); }
void run() {
    UiVrsGuard guard;
    uint64_t recordedActivation = 0;
    Context immediate, deferred;
    guard.configure(false, reset);
    check(!guard.invalidateCache(recordedActivation), "absent Magic does not invalidate coverage");
    guard.before(unknown(immediate), UiVrsScope::Layer);
    check(immediate.calls == 0 && immediate.coarse, "Magic absent leaves world and API untouched");
    guard.configure(true, reset);
    check(guard.invalidateCache(recordedActivation) && !guard.invalidateCache(recordedActivation), "late API activation invalidates each cache exactly once");
    guard.configure(true, reset);
    check(!guard.invalidateCache(recordedActivation), "unchanged module does not invalidate a protected cache");
    guard.before(nullptr, UiVrsScope::Layer);
    check(guard.counts(UiVrsScope::Layer).attempts == 0, "null context has no attempted reset");
    for (unsigned i = 0; i < static_cast<unsigned>(UiVrsScope::Count); ++i) {
        auto scope = static_cast<UiVrsScope>(i);
        guard.before(unknown(immediate), scope);
        const auto& n = guard.counts(scope);
        check(n.attempts == 1 && n.ok == 1 && n.errors == 0, "each scope accounts NVAPI_OK");
    }
    check(!immediate.coarse && immediate.srrv == 73, "UI is full rate and SRRV is preserved");
    immediate.coarse = true; // Magic's next accepted world pre-draw reinstates its own rates.
    check(immediate.coarse, "guard does not own or change the next world pre-draw");
    for (bool replayExtension : {false, true}) {
        immediate.coarse = deferred.coarse = true;
        guard.before(unknown(deferred), UiVrsScope::DeferredRecord);
        const bool recordedRate = deferred.coarse;
        bool sawFullRate = false, restoreForwarded = false;
        auto before = [&](Context* c, UiVrsScope scope) { guard.before(unknown(*c), scope); };
        auto execute = [&](Context* c, const bool* rate, int restore) {
            if (replayExtension) c->coarse = *rate;
            sawFullRate = !c->coarse;
            restoreForwarded = restore == 1;
            c->coarse = true; // model a vendor state restore unrelated to the D3D11 recorder
        };
        uiVrsExecute(&immediate, &recordedRate, 1, before, execute);
        check(sawFullRate && !immediate.coarse && restoreForwarded, "record, playback and following UI are protected with either extension-state model");
        check(immediate.srrv == 73 && deferred.srrv == 73, "command-list guard never unbinds either SRRV");
    }
    immediate.status = -104;
    guard.before(unknown(immediate), UiVrsScope::Tone);
    const auto& error = guard.counts(UiVrsScope::Tone);
    check(error.attempts == 2 && error.ok == 1 && error.errors == 1 && error.lastStatus == -104, "driver failure is visible without being counted as protection");
    guard.configure(true, nullptr);
    guard.before(unknown(immediate), UiVrsScope::Layer);
    check(guard.counts(UiVrsScope::Layer).errors == 1 && guard.counts(UiVrsScope::Layer).lastStatus == -3, "missing NVAPI entry reports unavailable");
    guard.configure(false, reset);
    const int calls = immediate.calls;
    guard.before(unknown(immediate), UiVrsScope::Layer);
    check(immediate.calls == calls, "unloading Magic disables the compatibility path");
    guard.configure(true, reset);
    check(guard.invalidateCache(recordedActivation) && !guard.invalidateCache(recordedActivation), "reactivation records one fresh disable");
}
}
int main(int argc, char** argv) {
    if (argc != 2 || (std::strcmp(argv[1], "--self-test") && std::strcmp(argv[1], "--dry-run"))) return 2;
    if (!std::strcmp(argv[1], "--dry-run")) { std::puts("UI VRS guard: fake NVAPI, no graphics driver or game"); return 0; }
    run();
    std::printf("UI VRS guard: %d failures\n", failures);
    return failures ? 1 : 0;
}
