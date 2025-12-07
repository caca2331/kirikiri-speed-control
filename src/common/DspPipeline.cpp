#include "DspPipeline.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>

#ifdef USE_SOUNDTOUCH
#include <soundtouch/SoundTouch.h>
#endif

namespace krkrspeed {

struct DspPipeline::Impl {
#ifdef USE_SOUNDTOUCH
    soundtouch::SoundTouch touch;
#endif
    std::mutex mutex;
    std::vector<std::uint8_t> scratch;
};

DspPipeline::DspPipeline(std::uint32_t sampleRate, std::uint32_t channels, const DspConfig &config)
    : m_sampleRate(sampleRate), m_channels(channels), m_config(config), m_impl(std::make_unique<Impl>()) {
#ifdef USE_SOUNDTOUCH
    m_impl->touch.setSampleRate(sampleRate);
    m_impl->touch.setChannels(static_cast<unsigned int>(channels));
    m_impl->touch.setSetting(SETTING_SEQUENCE_MS, static_cast<int>(config.sequenceMs));
    m_impl->touch.setSetting(SETTING_OVERLAP_MS, static_cast<int>(config.overlapMs));
    m_impl->touch.setSetting(SETTING_SEEKWINDOW_MS, static_cast<int>(config.seekWindowMs));
#endif
}

DspPipeline::~DspPipeline() = default;

std::vector<std::uint8_t> DspPipeline::process(const std::uint8_t *data, std::size_t bytes, float speedRatio) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const std::size_t sampleCount = bytes / sizeof(std::int16_t);

#ifdef USE_SOUNDTOUCH
    m_impl->touch.setTempo(speedRatio);
    m_impl->touch.putSamples(reinterpret_cast<const soundtouch::SAMPLETYPE *>(data), sampleCount / m_channels);

    std::vector<std::uint8_t> output;
    output.reserve(bytes);

    const std::size_t maxSamples = static_cast<std::size_t>(std::ceil(sampleCount / std::max(0.1f, speedRatio)) + 1024);
    m_impl->scratch.resize(maxSamples * sizeof(std::int16_t));

    const auto received = m_impl->touch.receiveSamples(reinterpret_cast<soundtouch::SAMPLETYPE *>(m_impl->scratch.data()), maxSamples / m_channels);
    output.insert(output.end(), m_impl->scratch.begin(), m_impl->scratch.begin() + received * m_channels * sizeof(std::int16_t));
    return output;
#else
    // Fallback path: perform naive resampling to approximate tempo without pitch preservation.
    if (sampleCount < 2 || std::abs(speedRatio - 1.0f) < 0.01f) {
        return std::vector<std::uint8_t>(data, data + bytes);
    }

    const double inv = 1.0 / speedRatio;
    const std::size_t outputSamples = static_cast<std::size_t>(sampleCount * inv);
    std::vector<std::uint8_t> output(outputSamples * sizeof(std::int16_t));

    const auto *input = reinterpret_cast<const std::int16_t *>(data);
    auto *out = reinterpret_cast<std::int16_t *>(output.data());
    for (std::size_t i = 0; i < outputSamples; ++i) {
        const double srcIndex = i * speedRatio;
        const std::size_t idx = static_cast<std::size_t>(srcIndex);
        const std::size_t next = std::min<std::size_t>(idx + 1, sampleCount - 1);
        const double frac = srcIndex - idx;
        const std::int16_t a = input[idx];
        const std::int16_t b = input[next];
        out[i] = static_cast<std::int16_t>(a + (b - a) * frac);
    }
    return output;
#endif
}

} // namespace krkrspeed
