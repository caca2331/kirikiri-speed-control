#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace krkrspeed {

struct BufferMeta {
    std::vector<std::uint8_t> payload;
    std::uint64_t frames = 0;
};

struct VoiceContext {
    float userSpeed = 1.0f;
    float engineRatio = 1.0f;
    float effectiveSpeed = 1.0f;
    bool isVoice = true;
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;

    std::deque<BufferMeta> pendingBuffers;
};

} // namespace krkrspeed
