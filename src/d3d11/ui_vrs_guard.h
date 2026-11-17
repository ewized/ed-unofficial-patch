#pragma once
// Magic4All can enable VRS outside EDUP's draw hook, before EDUP redirects UI.
// The documented zero-viewport NVAPI descriptor disables it without changing
// the shading-rate resource view. Magic reinstates its own rates at its next draw.
// ABI source: NVIDIA/nvapi nvapi.h, NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_V1,
// https://docs.nvidia.com/nvapi/group__dx.html
#include <cstddef>
#include <cstdint>

struct IUnknown;

namespace edvr {
struct UiVrsResetDesc {
    uint32_t version = 0x10010; // MAKE_NVAPI_VERSION(16-byte descriptor, 1), Win64
    uint32_t numViewports = 0;
    void* pViewports = nullptr; // ignored when numViewports is zero
};
static_assert(sizeof(UiVrsResetDesc) == 16 && offsetof(UiVrsResetDesc, pViewports) == 8,
              "NVAPI's Win64 viewport descriptor ABI");
using UiVrsReset = int (__cdecl*)(IUnknown*, UiVrsResetDesc*);

enum class UiVrsScope : unsigned { Layer, Tone, WriteBack, DeferredRecord, ExecuteBefore, ExecuteAfter, Count };
inline const char* uiVrsScopeName(UiVrsScope scope) {
    static const char* const names[] = {"layer", "tone", "write_back", "deferred_record", "execute_before", "execute_after"};
    return names[static_cast<unsigned>(scope)];
}
struct UiVrsCounts {
    uint64_t attempts = 0, ok = 0, errors = 0;
    int lastStatus = 0;
};
// Separate from module discovery and logging so a fake NVAPI can prove the
// exact descriptor, context routing and error accounting without a driver.
class UiVrsGuard {
    bool magic_ = false;
    UiVrsReset reset_ = nullptr;
    uint64_t activation_ = 0;
    UiVrsCounts counts_[static_cast<unsigned>(UiVrsScope::Count)]{};
public:
    void configure(bool magic, UiVrsReset reset) {
        if (magic && reset && reset != reset_) ++activation_;
        magic_ = magic;
        reset_ = magic ? reset : nullptr;
    }
    bool magicLoaded() const { return magic_; }
    bool apiReady() const { return reset_ != nullptr; }
    bool invalidateCache(uint64_t& recordedActivation) const {
        if (!reset_ || recordedActivation == activation_) return false;
        recordedActivation = activation_;
        return true;
    }
    const UiVrsCounts& counts(UiVrsScope scope) const { return counts_[static_cast<unsigned>(scope)]; }
    void before(IUnknown* ctx, UiVrsScope scope) {
        if (!magic_ || !ctx) return;
        UiVrsCounts& c = counts_[static_cast<unsigned>(scope)];
        ++c.attempts;
        UiVrsResetDesc desc;
        // NVAPI_NO_IMPLEMENTATION (-3) also makes an unavailable entry visible.
        c.lastStatus = reset_ ? reset_(ctx, &desc) : -3;
        if (c.lastStatus == 0) ++c.ok; else ++c.errors;
    }
};
template<class Context, class List, class Before, class Execute>
inline void uiVrsExecute(Context* ctx, List* list, int restore, Before before, Execute execute) {
    before(ctx, UiVrsScope::ExecuteBefore);
    execute(ctx, list, restore);
    before(ctx, UiVrsScope::ExecuteAfter);
}
} // namespace edvr
