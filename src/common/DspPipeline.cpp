#include "DspPipeline.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <type_traits>

#ifdef USE_SOUNDTOUCH
#include <soundtouch/SoundTouch.h>
#endif

namespace krkrspeed {

struct DspPipeline::Impl {
#ifdef USE_SOUNDTOUCH
    soundtouch::SoundTouch touch;
    std::vector<soundtouch::SAMPLETYPE> scratch;
#endif
    std::mutex mutex;
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

std::vector<std::uint8_t> DspPipeline::process(const std::uint8_t *data, std::size_t bytes, float speedRatio,
                                              DspMode mode) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (bytes == 0 || m_channels == 0) {
        return {};
    }

    const float tempo = std::max(0.01f, speedRatio);
    const float pitch = std::max(0.01f, speedRatio);
    const std::size_t sampleCount = bytes / sizeof(std::int16_t);
    const std::size_t frameCount = sampleCount / m_channels;
    if (frameCount == 0) {
        return {};
    }

#ifdef USE_SOUNDTOUCH
    using SampleType = soundtouch::SAMPLETYPE;
    constexpr bool kIsFloat = std::is_same_v<SampleType, float>;

    const auto *pcm = reinterpret_cast<const std::int16_t *>(data);

    // Prepare input for SoundTouch
    std::vector<SampleType> input(sampleCount);
    if constexpr (kIsFloat) {
        constexpr float invShortMax = 1.0f / 32768.0f;
        for (std::size_t i = 0; i < sampleCount; ++i) {
            input[i] = static_cast<float>(pcm[i]) * invShortMax;
        }
    } else {
        std::memcpy(input.data(), pcm, sampleCount * sizeof(std::int16_t));
    }

    if (mode == DspMode::Tempo) {
        m_impl->touch.setTempo(tempo);
        m_impl->touch.setRate(1.0f);
        m_impl->touch.setPitch(1.0f);
    } else {
        m_impl->touch.setTempo(1.0f);
        m_impl->touch.setRate(1.0f);
        m_impl->touch.setPitch(pitch);
    }

    m_impl->touch.putSamples(input.data(), frameCount);

    std::vector<std::uint8_t> output;

    const std::size_t maxFrames = static_cast<std::size_t>(std::ceil(frameCount / std::max(0.1f, tempo)) + 1024);
    m_impl->scratch.resize(maxFrames * m_channels);

    while (true) {
        const auto receivedFrames = m_impl->touch.receiveSamples(m_impl->scratch.data(), maxFrames);
        if (receivedFrames == 0) break;
        const std::size_t receivedSamples = receivedFrames * m_channels;

        if constexpr (kIsFloat) {
            std::size_t prev = output.size();
            output.resize(prev + receivedSamples * sizeof(std::int16_t));
            auto *outPcm = reinterpret_cast<std::int16_t *>(output.data() + prev);
            for (std::size_t i = 0; i < receivedSamples; ++i) {
                const float clamped = std::clamp(static_cast<float>(m_impl->scratch[i]), -1.0f, 1.0f);
                outPcm[i] = static_cast<std::int16_t>(std::lround(clamped * 32767.0f));
            }
        } else {
            std::size_t prev = output.size();
            output.resize(prev + receivedSamples * sizeof(std::int16_t));
            std::memcpy(output.data() + prev, m_impl->scratch.data(), receivedSamples * sizeof(std::int16_t));
        }
        if (output.size() >= bytes && mode == DspMode::Tempo) break;
        // Logic for break: if we got enough samples.
        // Note: original logic had `if (output.size() >= bytes) break;`.
    }

    if (output.empty()) {
        auto naiveResample = [&](float ratio) {
            const double inv = 1.0 / ratio;
            const std::size_t outputSamples = static_cast<std::size_t>(sampleCount * inv);
            std::vector<std::uint8_t> outBytes(outputSamples * sizeof(std::int16_t));
            const auto *input = reinterpret_cast<const std::int16_t *>(data);
            auto *out = reinterpret_cast<std::int16_t *>(outBytes.data());
            for (std::size_t i = 0; i < outputSamples; ++i) {
                const double srcIndex = i * ratio;
                const std::size_t idx = static_cast<std::size_t>(srcIndex);
                const std::size_t next = std::min<std::size_t>(idx + 1, sampleCount - 1);
                const double frac = srcIndex - idx;
                const std::int16_t a = input[idx];
                const std::int16_t b = input[next];
                out[i] = static_cast<std::int16_t>(a + (b - a) * frac);
            }
            return outBytes;
        };
        if (mode == DspMode::Tempo && std::abs(speedRatio - 1.0f) >= 0.01f) {
            return naiveResample(speedRatio);
        }
        if (mode == DspMode::Pitch) {
            return std::vector<std::uint8_t>(data, data + bytes);
        }
    }

    return output;
#else
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

std::vector<float> DspPipeline::process(const float *data, std::size_t samples, float speedRatio, DspMode mode) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (samples == 0 || m_channels == 0) {
        return {};
    }

    const float tempo = std::max(0.01f, speedRatio);
    const float pitch = std::max(0.01f, speedRatio);
    const std::size_t frameCount = samples / m_channels;
    if (frameCount == 0) {
        return {};
    }

#ifdef USE_SOUNDTOUCH
    using SampleType = soundtouch::SAMPLETYPE;
    constexpr bool kIsFloat = std::is_same_v<SampleType, float>;

    // Prepare input
    std::vector<SampleType> input(samples);
    if constexpr (kIsFloat) {
        std::memcpy(input.data(), data, samples * sizeof(float));
    } else {
        // Convert float to int16 (SampleType=int16)
        for(size_t i=0; i<samples; ++i) {
             const float clamped = std::clamp(data[i], -1.0f, 1.0f);
             input[i] = static_cast<std::int16_t>(std::lround(clamped * 32767.0f));
        }
    }

    if (mode == DspMode::Tempo) {
        m_impl->touch.setTempo(tempo);
        m_impl->touch.setRate(1.0f);
        m_impl->touch.setPitch(1.0f);
    } else {
        m_impl->touch.setTempo(1.0f);
        m_impl->touch.setRate(1.0f);
        m_impl->touch.setPitch(pitch);
    }

    m_impl->touch.putSamples(input.data(), frameCount);

    std::vector<float> output;
    const std::size_t maxFrames = static_cast<std::size_t>(std::ceil(frameCount / std::max(0.1f, tempo)) + 1024);
    m_impl->scratch.resize(maxFrames * m_channels);

    while (true) {
        const auto receivedFrames = m_impl->touch.receiveSamples(m_impl->scratch.data(), maxFrames);
        if (receivedFrames == 0) break;
        const std::size_t receivedSamples = receivedFrames * m_channels;

        std::size_t prev = output.size();
        output.resize(prev + receivedSamples);

        if constexpr (kIsFloat) {
            for(size_t i=0; i<receivedSamples; ++i) {
                output[prev + i] = static_cast<float>(m_impl->scratch[i]);
            }
        } else {
            constexpr float invShortMax = 1.0f / 32768.0f;
            for(size_t i=0; i<receivedSamples; ++i) {
                output[prev + i] = static_cast<float>(m_impl->scratch[i]) * invShortMax;
            }
        }
    }

    if (output.empty()) {
        auto naiveResample = [&](float ratio) {
            const double inv = 1.0 / ratio;
            const std::size_t outputSamples = static_cast<std::size_t>(samples * inv);
            std::vector<float> out(outputSamples);
            for (std::size_t i = 0; i < outputSamples; ++i) {
                const double srcIndex = i * ratio;
                const std::size_t idx = static_cast<std::size_t>(srcIndex);
                const std::size_t next = std::min<std::size_t>(idx + 1, samples - 1);
                const double frac = srcIndex - idx;
                const float a = data[idx];
                const float b = data[next];
                out[i] = static_cast<float>(a + (b - a) * frac);
            }
            return out;
        };
        if (mode == DspMode::Tempo && std::abs(speedRatio - 1.0f) >= 0.01f) {
            return naiveResample(speedRatio);
        }
        if (mode == DspMode::Pitch) {
            return std::vector<float>(data, data + samples);
        }
    }
    return output;
#else
    if (samples < 2 || std::abs(speedRatio - 1.0f) < 0.01f) {
        return std::vector<float>(data, data + samples);
    }
    const double inv = 1.0 / speedRatio;
    const std::size_t outputSamples = static_cast<std::size_t>(samples * inv);
    std::vector<float> output(outputSamples);
    for (std::size_t i = 0; i < outputSamples; ++i) {
        const double srcIndex = i * speedRatio;
        const std::size_t idx = static_cast<std::size_t>(srcIndex);
        const std::size_t next = std::min<std::size_t>(idx + 1, samples - 1);
        const double frac = srcIndex - idx;
        const float a = data[idx];
        const float b = data[next];
        output[i] = static_cast<float>(a + (b - a) * frac);
    }
    return output;
#endif
}

} // namespace krkrspeed
