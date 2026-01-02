#include "AudioStreamProcessor.h"
#include "Logging.h"

#include <algorithm>

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

    const std::size_t cap =
        std::max<std::size_t>(m_blockAlign ? m_blockAlign : 1, static_cast<std::size_t>(bytesPerSec * 0.1));
    if (m_cbuffer.size() > cap) {
        const size_t overflow = m_cbuffer.size() - cap;
        // Keep earliest data; drop newest overflow to preserve continuity.
        m_cbuffer.resize(cap);
        KRKR_LOG_WARN("AudioStream: cbuffer overflow trimmed overflow=" + std::to_string(overflow) +
                      " cap=" + std::to_string(cap) +
                      " key=" + std::to_string(key));
    }

    result.cbufferSize = m_cbuffer.size();
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
