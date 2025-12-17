#pragma once

#include <cstdint>
#include <string>

namespace krkrspeed {

struct SharedSettings {
    float userSpeed = 2.0f;
    float lengthGateSeconds = 60.0f;
    std::uint32_t lengthGateEnabled = 1;
    std::uint32_t enableLog = 0;
    std::uint32_t skipDirectSound = 0;
    std::uint32_t skipXAudio2 = 0;
    std::uint32_t skipFmod = 0;
    std::uint32_t skipWwise = 0;
    std::uint32_t safeMode = 0;
    std::uint32_t disableBgm = 0;
    std::uint32_t processAllAudio = 0;
    float bgmSecondsGate = 60.0f;
    std::uint32_t stereoBgmMode = 1; // 0=aggressive,1=hybrid(default),2=none
};

inline std::wstring BuildSharedSettingsName(std::uint32_t pid) {
    return L"Local\\KrkrSpeedSettings_" + std::to_wstring(pid);
}

} // namespace krkrspeed
