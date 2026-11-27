#include "frame_generation_coordinator.h"

static_assert(sizeof(EdvrFgSource)==144, "FG source ABI");
static_assert(sizeof(EdvrFgStatus)==104, "FG status ABI");
static_assert(edvrFgBatchSize(EDVR_FG_BATCH_VERSION_2)==392, "FG batch V2 prefix ABI");
static_assert(edvrFgBatchSize(EDVR_FG_BATCH_VERSION_3)==408, "FG batch V3 prefix ABI");
static_assert(offsetof(EdvrFgBatch,allocationId)==408&&sizeof(EdvrFgBatch)==416, "FG batch V4 ABI");

extern "C" HRESULT WINAPI edvrAcquireNativeFrameGeneration(
    const EdvrFgRequest* request, EdvrFgTable* table) {
  return edvr::nativeFgAcquire(request, table);
}
