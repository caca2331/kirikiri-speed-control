#pragma once

#include <cstdint>
#include <string>

namespace krkrspeed {

struct SharedSettings {
    float userSpeed = 2.0f;
    float lengthGateSeconds = 60.0f;
    std::uint32_t lengthGateEnabled = 1;
    std::uint32_t version = 2;
    std::uint32_t enableLog = 0;
    std::uint32_t skipDirectSound = 0;
    std::uint32_t skipXAudio2 = 0;
    std::uint32_t safeMode = 0;
    std::uint32_t disableVeh = 0;
    std::uint32_t disableBgm = 0;
    std::uint32_t forceAll = 0;
    float bgmSecondsGate = 60.0f;
};

inline std::wstring BuildSharedSettingsName(std::uint32_t pid) {
    return L"Local\\KrkrSpeedSettings_" + std::to_wstring(pid);
}

} // namespace krkrspeed
