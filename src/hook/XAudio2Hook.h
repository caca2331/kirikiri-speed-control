#pragma once

#include "../common/DspPipeline.h"
#include "../common/VoiceContext.h"
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <windows.h>

namespace krkrspeed {

class XAudio2Hook {
public:
    static XAudio2Hook &instance();
    void initialize();

    void setUserSpeed(float speed);
    void configureLengthGate(bool enabled, float seconds);

private:
    XAudio2Hook() = default;
    void detectVersion();
    void hookEntryPoints();
    void onCreateSourceVoice(std::uintptr_t voiceKey, std::uint32_t sampleRate, std::uint32_t channels);
    std::vector<std::uint8_t> onSubmitBuffer(std::uintptr_t voiceKey, const std::uint8_t *data, std::size_t size);

    float m_userSpeed = 1.0f;
    bool m_lengthGateEnabled = true;
    float m_lengthGateSeconds = 30.0f;
    std::map<std::uintptr_t, VoiceContext> m_contexts;
    std::mutex m_mutex;
    std::string m_version;
};

} // namespace krkrspeed
