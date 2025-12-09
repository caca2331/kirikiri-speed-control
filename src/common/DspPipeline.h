#pragma once

#include "VoiceContext.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace krkrspeed {

struct DspConfig {
    float sequenceMs = 35.0f;
    float overlapMs = 10.0f;
    float seekWindowMs = 25.0f;
};

enum class DspMode {
    Tempo,   // change tempo (speed) while keeping pitch
    Pitch    // change pitch while keeping tempo
};

class DspPipeline {
public:
    explicit DspPipeline(std::uint32_t sampleRate, std::uint32_t channels, const DspConfig &config = {});
    ~DspPipeline();

    DspPipeline(const DspPipeline &) = delete;
    DspPipeline &operator=(const DspPipeline &) = delete;

    // Returns processed PCM bytes. Input samples are expected to be 16-bit PCM.
    // mode=Tempo: adjust tempo by speedRatio (pitch preserved).
    // mode=Pitch: adjust pitch by speedRatio (tempo preserved).
    std::vector<std::uint8_t> process(const std::uint8_t *data, std::size_t bytes, float speedRatio,
                                      DspMode mode = DspMode::Tempo);

    std::uint32_t sampleRate() const { return m_sampleRate; }
    std::uint32_t channels() const { return m_channels; }
    const DspConfig &config() const { return m_config; }

private:
    std::uint32_t m_sampleRate;
    std::uint32_t m_channels;
    DspConfig m_config;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace krkrspeed
