#include "AudioStreamProcessor.h"
#include "Logging.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace krkrspeed {

AudioStreamProcessor::AudioStreamProcessor(std::uint32_t sampleRate, std::uint32_t channels, std::uint32_t blockAlign,
                                           const DspConfig &cfg)
    : m_sampleRate(sampleRate), m_blockAlign(blockAlign) {
    m_dsp = std::make_unique<DspPipeline>(sampleRate, channels, cfg);
    if (m_blockAlign == 0 && channels > 0) {
        m_blockAlign = channels * sizeof(std::int16_t);
    }
}

AudioProcessResult AudioStreamProcessor::process(const std::uint8_t *data, std::size_t bytes, float userSpeed,
                                                 bool shouldLog, std::uintptr_t key) {
    AudioProcessResult result;
    auto fillPassthrough = [&](float appliedSpeed) {
        if (data && bytes > 0) {
            result.output.assign(data, data + bytes);
        }
        result.cbufferSize = m_cbuffer.size();
        result.appliedSpeed = appliedSpeed;
    };
    if (!data || bytes == 0 || !m_dsp) {
        fillPassthrough(1.0f);
        return result;
    }
    if (bytes < 10) {
        // Ignore tiny buffers entirely.
        fillPassthrough(userSpeed);
        return result;
    }

    const float denom = std::max(0.01f, userSpeed);
    const float pitchDown = 1.0f / denom;

    const std::size_t bytesPerSec = std::max<std::size_t>(1, m_blockAlign * m_sampleRate);
    result.output.reserve(bytes);
    std::size_t need = bytes;

    if (m_padNext) {
        const double durationSec = static_cast<double>(bytes) / static_cast<double>(bytesPerSec);
        if (durationSec < 1.01) {
            const std::size_t align = m_blockAlign ? m_blockAlign : 1;
            std::size_t padBytesTarget = static_cast<std::size_t>(bytesPerSec * 0.03);
            padBytesTarget = (padBytesTarget / align) * align;
            if (padBytesTarget == 0) padBytesTarget = align;
            const std::size_t padBytes = std::min(padBytesTarget, bytes);
            result.output.insert(result.output.end(), padBytes, 0); // zero padding
            need -= padBytes;
            if (shouldLog) {
                KRKR_LOG_DEBUG("AudioStream: initial front-pad " + std::to_string(padBytes) +
                               " bytes key=" + std::to_string(key));
            }
        }
        m_padNext = false;
    }

    // 1) Consume already-processed tail first; do not re-run through DSP.
    if (!m_cbuffer.empty()) {
        const std::size_t take = std::min(m_cbuffer.size(), need);
        result.output.insert(result.output.end(), m_cbuffer.begin(), m_cbuffer.begin() + take);
        m_cbuffer.erase(m_cbuffer.begin(), m_cbuffer.begin() + take);
        need -= take;
    }

    // 2) Always process new input; if output already filled, stash everything to cbuffer.
    auto out = m_dsp->process(data, bytes, pitchDown, DspMode::Pitch);
    if (out.empty()) {
        if (shouldLog) {
            KRKR_LOG_DEBUG("AudioStream: pitch-compensate produced 0 bytes; passthrough key=" +
                           std::to_string(key));
        }
        out.assign(data, data + bytes);
    }
    if (need > 0) {
        const std::size_t take = std::min<std::size_t>(need, out.size());
        result.output.insert(result.output.end(), out.begin(), out.begin() + take);
        need -= take;
        if (out.size() > take) {
            m_cbuffer.insert(m_cbuffer.end(), out.begin() + take, out.end());
        }
    } else if (!out.empty()) {
        m_cbuffer.insert(m_cbuffer.end(), out.begin(), out.end());
    }

    // 3) Front-pad if still short to satisfy exact Abuffer length (per new requirement).
    if (need > 0) {
        result.output.insert(result.output.begin(), need, 0);
        if (shouldLog) {
            KRKR_LOG_DEBUG("AudioStream: front-padded " + std::to_string(need) +
                           " bytes (pitch) key=" + std::to_string(key));
        }
        need = 0;
    }

    result.cbufferSize = m_cbuffer.size();
    result.appliedSpeed = userSpeed;
    m_lastAppliedSpeed = result.appliedSpeed;
    return result;
}

AudioProcessResult AudioStreamProcessor::processTempoToSize(const std::uint8_t *data, std::size_t inputBytes,
                                                            std::size_t outputBytes, float userSpeed, bool shouldLog,
                                                            std::uintptr_t key) {
    AudioProcessResult result;
    if (outputBytes == 0) {
        result.appliedSpeed = userSpeed;
        return result;
    }

    const float appliedSpeed = userSpeed <= 0.01f ? 1.0f : userSpeed;
    const std::size_t bytesPerSec = std::max<std::size_t>(1, m_blockAlign * m_sampleRate);
    result.output.reserve(outputBytes);
    std::size_t need = outputBytes;

    if (m_padNext) {
        const double durationSec =
            bytesPerSec > 0 ? static_cast<double>(inputBytes) / static_cast<double>(bytesPerSec) : 0.0;
        if (durationSec < 1.01) {
            const std::size_t align = m_blockAlign ? m_blockAlign : 1;
            std::size_t padBytesTarget = static_cast<std::size_t>(bytesPerSec * 0.03);
            padBytesTarget = (padBytesTarget / align) * align;
            if (padBytesTarget == 0) padBytesTarget = align;
            const std::size_t padBytes = std::min(padBytesTarget, need);
            result.output.insert(result.output.end(), padBytes, 0);
            need -= padBytes;
            if (shouldLog) {
                KRKR_LOG_DEBUG("AudioStream: initial front-pad " + std::to_string(padBytes) +
                               " bytes key=" + std::to_string(key));
            }
        }
        m_padNext = false;
    }

    if (!m_cbuffer.empty()) {
        const std::size_t take = std::min(m_cbuffer.size(), need);
        result.output.insert(result.output.end(), m_cbuffer.begin(), m_cbuffer.begin() + take);
        m_cbuffer.erase(m_cbuffer.begin(), m_cbuffer.begin() + take);
        need -= take;
    }

    if (data && inputBytes > 0) {
        m_abuffer.insert(m_abuffer.end(), data, data + inputBytes);
    }

    std::vector<std::uint8_t> processed;
    if (m_dsp && !m_abuffer.empty()) {
        const std::size_t align = m_blockAlign ? m_blockAlign : 1;
        std::size_t minBytes = static_cast<std::size_t>(bytesPerSec * 0.03);
        minBytes = (minBytes / align) * align;
        if (minBytes == 0) minBytes = align;
        if (m_abuffer.size() >= minBytes) {
            processed = m_dsp->process(m_abuffer.data(), m_abuffer.size(), appliedSpeed, DspMode::Tempo);
            m_abuffer.clear();
            if (processed.empty() && shouldLog) {
                KRKR_LOG_DEBUG("AudioStream: tempo produced 0 bytes; holding output key=" +
                               std::to_string(key));
            }
        }
    }

    if (!processed.empty()) {
        if (need > 0) {
            const std::size_t take = std::min<std::size_t>(need, processed.size());
            result.output.insert(result.output.end(), processed.begin(), processed.begin() + take);
            need -= take;
            if (processed.size() > take) {
                m_cbuffer.insert(m_cbuffer.end(), processed.begin() + take, processed.end());
            }
        } else {
            m_cbuffer.insert(m_cbuffer.end(), processed.begin(), processed.end());
        }
    }

    if (need > 0) {
        result.output.insert(result.output.end(), need, 0);
        if (shouldLog) {
            KRKR_LOG_DEBUG("AudioStream: tail-padded " + std::to_string(need) +
                           " bytes (tempo) key=" + std::to_string(key));
        }
        need = 0;
    }

    result.cbufferSize = m_cbuffer.size();
    result.appliedSpeed = appliedSpeed;
    m_lastAppliedSpeed = result.appliedSpeed;
    return result;
}

AudioProcessResult AudioStreamProcessor::processPitchToSize(const std::uint8_t *data, std::size_t inputBytes,
                                                            std::size_t outputBytes, float userSpeed, bool shouldLog,
                                                            std::uintptr_t key) {
    AudioProcessResult result;
    if (outputBytes == 0) {
        result.appliedSpeed = userSpeed;
        return result;
    }

    if (!data || inputBytes == 0 || !m_dsp || m_blockAlign == 0) {
        result.output.assign(outputBytes, 0);
        result.cbufferSize = 0;
        result.appliedSpeed = userSpeed;
        m_lastAppliedSpeed = result.appliedSpeed;
        return result;
    }

    const std::size_t inFrames = inputBytes / m_blockAlign;
    const std::size_t outFrames = outputBytes / m_blockAlign;
    if (inFrames == 0 || outFrames == 0) {
        result.output.assign(outputBytes, 0);
        result.cbufferSize = 0;
        result.appliedSpeed = userSpeed;
        m_lastAppliedSpeed = result.appliedSpeed;
        return result;
    }

    const float ratio = static_cast<float>(outFrames) / static_cast<float>(inFrames);
    const float pitchDown = std::clamp(ratio, 0.01f, 4.0f);

    std::vector<std::uint8_t> processed = m_dsp->process(data, inputBytes, pitchDown, DspMode::Pitch);
    if (processed.empty()) {
        if (shouldLog) {
            KRKR_LOG_DEBUG("AudioStream: pitch process produced 0 bytes; fallback passthrough key=" +
                           std::to_string(key));
        }
        processed.assign(data, data + inputBytes);
    }

    const std::size_t procFrames = processed.size() / m_blockAlign;
    result.output.resize(outputBytes);

    if (procFrames == 0) {
        std::fill(result.output.begin(), result.output.end(), 0);
    } else if (procFrames == outFrames) {
        std::memcpy(result.output.data(), processed.data(), outputBytes);
    } else {
        const std::size_t channels = m_blockAlign / sizeof(std::int16_t);
        const auto *inSamples = reinterpret_cast<const std::int16_t *>(processed.data());
        auto *outSamples = reinterpret_cast<std::int16_t *>(result.output.data());
        const double ratio = (outFrames > 1)
            ? static_cast<double>(procFrames - 1) / static_cast<double>(outFrames - 1)
            : 0.0;

        for (std::size_t i = 0; i < outFrames; ++i) {
            const double src = ratio * static_cast<double>(i);
            const std::size_t idx = static_cast<std::size_t>(src);
            const std::size_t next = std::min(idx + 1, procFrames - 1);
            const double frac = src - static_cast<double>(idx);
            const std::size_t base = idx * channels;
            const std::size_t baseNext = next * channels;
            const std::size_t outBase = i * channels;

            for (std::size_t c = 0; c < channels; ++c) {
                const std::int16_t a = inSamples[base + c];
                const std::int16_t b = inSamples[baseNext + c];
                const double sample = static_cast<double>(a) + (static_cast<double>(b) - static_cast<double>(a)) * frac;
                outSamples[outBase + c] = static_cast<std::int16_t>(std::lround(sample));
            }
        }
    }

    m_cbuffer.clear();
    m_padNext = false;
    result.cbufferSize = 0;
    result.appliedSpeed = userSpeed;
    m_lastAppliedSpeed = result.appliedSpeed;
    return result;
}

void AudioStreamProcessor::resetIfIdle(std::chrono::steady_clock::time_point now,
                                       std::chrono::milliseconds idleThreshold, bool shouldLog, std::uintptr_t key) {
    if (m_lastPlayEnd.time_since_epoch().count() == 0) return;
    const auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastPlayEnd);
    if (idleMs > idleThreshold) {
        if (!m_cbuffer.empty() && shouldLog) {
            KRKR_LOG_DEBUG("AudioStream: stream reset after idle gap key=" + std::to_string(key) +
                           " idleMs=" + std::to_string(idleMs.count()));
        }
        m_cbuffer.clear();
        m_abuffer.clear();
        m_padNext = true;
        if (m_dsp) {
            m_dsp->flush();
        }
    }
}

void AudioStreamProcessor::recordPlaybackEnd(float durationSec, float appliedSpeed) {
    const float applied = appliedSpeed > 0.01f ? appliedSpeed : 1.0f;
    const float playTime = durationSec / applied;
    m_lastPlayEnd = std::chrono::steady_clock::now() +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(playTime));
}

} // namespace krkrspeed
