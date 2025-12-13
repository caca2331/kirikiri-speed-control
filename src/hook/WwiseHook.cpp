#include "WwiseHook.h"
#include "../common/Logging.h"
#include <windows.h>

namespace krkrspeed {

WwiseHook& WwiseHook::instance() {
    static WwiseHook hook;
    return hook;
}

void WwiseHook::initialize() {
    if (GetModuleHandleA("AkSoundEngine.dll")) {
        KRKR_LOG_INFO("Wwise Audio Engine detected (AkSoundEngine.dll). Hooking is currently experimental/stubbed.");
        // Future: Scan for AK::SoundEngine::PostEvent and hook it to track voices.
    }
}

} // namespace krkrspeed
