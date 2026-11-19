#pragma once

#include "../common/dlss_quality.h"

namespace edvr {

// The quality applied by the active native VR host. Custom also means
// that the host declined the override or has not published sizing yet.
DlssQuality nativeDlssQualityActive() noexcept;

}
