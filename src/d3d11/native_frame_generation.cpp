#include "frame_generation_coordinator.h"

static_assert(sizeof(EdvrFgSource)==144, "FG source ABI");
static_assert(sizeof(EdvrFgStatus)==104, "FG status ABI");
static_assert(sizeof(EdvrFgBatch)==408, "FG batch V3 ABI");

extern "C" HRESULT WINAPI edvrAcquireNativeFrameGeneration(
    const EdvrFgRequest* request, EdvrFgTable* table) {
  return edvr::nativeFgAcquire(request, table);
}
