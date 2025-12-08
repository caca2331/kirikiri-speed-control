#include "XAudio2Hook.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <cstdint>
#include "../common/Logging.h"

namespace krkrspeed {

XAudio2Hook &XAudio2Hook::instance() {
    static XAudio2Hook hook;
    return hook;
}

void XAudio2Hook::initialize() {
    detectVersion();
    hookEntryPoints();
    KRKR_LOG_INFO("XAudio2 hook initialized for version " + m_version);
}

void XAudio2Hook::detectVersion() {
    HMODULE xa27 = GetModuleHandleA("XAudio2_7.dll");
    HMODULE xa28 = GetModuleHandleA("XAudio2_8.dll");
    HMODULE xa29 = GetModuleHandleA("XAudio2_9.dll");
    if (xa29) m_version = "2.9";
    else if (xa28) m_version = "2.8";
    else if (xa27) m_version = "2.7";
    else m_version = "unknown";
    KRKR_LOG_DEBUG("Detected XAudio2 version: " + m_version);
}

void XAudio2Hook::hookEntryPoints() {
    // Placeholder for MinHook wiring. The scaffolding allows future work to attach to
    // XAudio2Create/CoCreateInstance dynamically while keeping thread-safe state.
    KRKR_LOG_INFO("XAudio2 hookEntryPoints stub invoked (MinHook wiring pending)");
}

void XAudio2Hook::setUserSpeed(float speed) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_userSpeed = std::clamp(speed, 0.5f, 10.0f);
    KRKR_LOG_INFO("User speed set to " + std::to_string(m_userSpeed) + "x");
    for (auto &kv : m_contexts) {
        kv.second.userSpeed = m_userSpeed;
        kv.second.effectiveSpeed = kv.second.userSpeed * kv.second.engineRatio;
    }
}

void XAudio2Hook::configureLengthGate(bool enabled, float seconds) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lengthGateEnabled = enabled;
    m_lengthGateSeconds = std::clamp(seconds, 0.1f, 600.0f);
    KRKR_LOG_INFO(std::string("Length gate ") + (enabled ? "enabled" : "disabled") + " at " +
                  std::to_string(m_lengthGateSeconds) + "s");
}

void XAudio2Hook::onCreateSourceVoice(std::uintptr_t voiceKey, std::uint32_t sampleRate, std::uint32_t channels) {
    std::lock_guard<std::mutex> lock(m_mutex);
    VoiceContext ctx;
    ctx.sampleRate = sampleRate;
    ctx.channels = channels;
    ctx.effectiveSpeed = ctx.userSpeed * ctx.engineRatio;
    m_contexts.emplace(voiceKey, std::move(ctx));
    KRKR_LOG_DEBUG("Created voice context key=" + std::to_string(voiceKey) + " sr=" + std::to_string(sampleRate) +
                   " ch=" + std::to_string(channels));
}

std::vector<std::uint8_t> XAudio2Hook::onSubmitBuffer(std::uintptr_t voiceKey, const std::uint8_t *data, std::size_t size) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_contexts.find(voiceKey);
    if (it == m_contexts.end()) {
        return std::vector<std::uint8_t>(data, data + size);
    }

    const float ratio = it->second.effectiveSpeed;
    if (!it->second.isVoice) {
        return std::vector<std::uint8_t>(data, data + size);
    }

    if (m_lengthGateEnabled && it->second.sampleRate > 0 && it->second.channels > 0) {
        const std::size_t frames = (size / sizeof(std::int16_t)) / it->second.channels;
        const float durationSec = static_cast<float>(frames) / static_cast<float>(it->second.sampleRate);
        if (durationSec > m_lengthGateSeconds) {
            return std::vector<std::uint8_t>(data, data + size);
        }
    }

    // Lazy-initialize DSP pipeline per voice.
    static DspConfig defaultConfig{};
    static std::map<std::uintptr_t, std::unique_ptr<DspPipeline>> pipelines;
    auto pipeline = pipelines.find(voiceKey);
    if (pipeline == pipelines.end()) {
        const std::uint32_t sr = it->second.sampleRate > 0 ? it->second.sampleRate : 44100;
        const std::uint32_t ch = it->second.channels > 0 ? it->second.channels : 1;
        auto dsp = std::make_unique<DspPipeline>(sr, ch, defaultConfig);
        pipeline = pipelines.emplace(voiceKey, std::move(dsp)).first;
        KRKR_LOG_DEBUG("Initialized DSP pipeline for voice key=" + std::to_string(voiceKey));
    }
    return pipeline->second->process(data, size, ratio);
}

} // namespace krkrspeed
