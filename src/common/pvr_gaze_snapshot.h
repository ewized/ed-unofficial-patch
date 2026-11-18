#pragma once
#include <windows.h>
#include <cstdint>
#include <cstddef>

#define EDVR_PVR_GAZE_SNAPSHOT_VERSION_1 1u
// Diagnostic CPU ABI only. Gaze is the UNCHANGED returned PVR geometry, not
// calibrated per-eye rays. Keep coordinates in memory; never log/upload them.
// callbackQpc is publication/callback time, not a sensor sample timestamp.
struct EdvrPvrGazeSnapshotV1 {
    uint32_t size, version;
    uint64_t sequence, callbackQpc, qpcFrequency;
    float returnedGazeTan[2][2];
    uint32_t providerQueried, providerFiniteValid, cachedFallback, ignored;
    uint32_t trackerPresent, returnedValid, mode, invertY;
};
static_assert(sizeof(EdvrPvrGazeSnapshotV1)==80 &&
              offsetof(EdvrPvrGazeSnapshotV1,returnedGazeTan)==32 &&
              offsetof(EdvrPvrGazeSnapshotV1,providerQueried)==48,"gaze snapshot Win64 ABI");
using EdvrPvrSetGazeSnapshotEnabled = BOOL (WINAPI*)(uint32_t,BOOL);
using EdvrPvrGetGazeSnapshot = BOOL (WINAPI*)(uint32_t,uint32_t,void*);
// Exports: edvrPvrSetGazeSnapshotEnabled(version,enable),
// edvrPvrGetGazeSnapshot(version,sizeof(EdvrPvrGazeSnapshotV1),out).
// Default disabled; FALSE means unavailable (including lock contention).
// Getter performs no VR/gaze/clock query. Disable before performance capture.
// Disable TRUE also acknowledges publisher quiescence; FALSE requires retry.
// Fresh-provider evidence requires new sequence, returnedValid, providerQueried,
// providerFiniteValid, !cachedFallback, !ignored. Check callback age separately.
// invertY is the callback's control; cached geometry can predate that control.
// Without a tracker geometry is undefined in upstream: returnedValid=0 and the
// diagnostic coordinates are zero, never advertised as an actual valid gaze.
