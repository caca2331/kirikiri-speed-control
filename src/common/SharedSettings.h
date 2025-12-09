#pragma once

#include <cstdint>
#include <string>

namespace krkrspeed {

struct SharedSettings {
    float userSpeed = 1.0f;
    float lengthGateSeconds = 30.0f;
    std::uint32_t lengthGateEnabled = 1;
    std::uint32_t version = 1;
};

inline std::wstring BuildSharedSettingsName(std::uint32_t pid) {
    return L"Local\\KrkrSpeedSettings_" + std::to_wstring(pid);
}

} // namespace krkrspeed
