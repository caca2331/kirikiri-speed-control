#pragma once

#include <cstdint>
#include <string>

namespace krkrspeed {

enum class AudioBackend : std::uint32_t {
    Unknown = 0,
    DirectSound = 1,
    Wasapi = 2
};

struct SharedStatus {
    std::uint32_t activeBackend = 0;
    std::uint64_t lastUpdateMs = 0;
};

inline std::wstring BuildSharedStatusName(std::uint32_t pid) {
    return L"Local\\KrkrSpeedStatus_" + std::to_wstring(pid);
}

} // namespace krkrspeed
