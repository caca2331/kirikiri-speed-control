#pragma once

#include <mutex>
#include <unordered_map>
#include <string>
#include <memory>
#include <vector>
#include "../common/SharedSettings.h"
#include "../common/DspPipeline.h"

namespace krkrspeed {

class FMODHook {
public:
    static FMODHook& instance();
    void initialize();
    void pollSharedSettings();
    void setUserSpeed(float speed);

    void setOriginalSystemPlaySound(void* fn) { m_origSystemPlaySound = fn; }
    static void* getSystemPlaySoundHook();

    // Callback helpers
    void cleanupChannel(void* channel);
    void* getOriginalCallback(void* channel);

private:
    FMODHook() = default;

    void scanLoadedModules();
    void installHooks(void* moduleHandle);

    // Helpers
    void onPlaySound(void* channel, void* sound, void* system);

    // Pointers to FMOD functions
    void* m_fnSystemPlaySound = nullptr;
    void* m_fnSystemCreateDSP = nullptr;
    void* m_fnDSPRelease = nullptr;
    void* m_fnChannelAddDSP = nullptr;
    void* m_fnChannelSetFrequency = nullptr;
    void* m_fnChannelGetFrequency = nullptr;
    void* m_fnChannelSetPaused = nullptr;
    void* m_fnChannelSetCallback = nullptr;

    // Original function trampolines
    void* m_origSystemPlaySound = nullptr;
    void* m_origChannelSetCallback = nullptr;

    float m_userSpeed = 1.0f;
    std::mutex m_mutex;

    // Shared settings
    void* m_sharedMapping = nullptr;
    SharedSettings* m_sharedView = nullptr;

    struct ChannelInfo {
        float baseFrequency = 0.0f;
        void* dsp = nullptr; // The custom DSP we attached
        void* originalCallback = nullptr;
    };
    std::unordered_map<std::uintptr_t, ChannelInfo> m_channels;

    // Hook implementation
    static int __stdcall SystemPlaySoundHook(void* system, void* channelgroup, void* sound, int paused, void** channel);
    static int __stdcall ChannelSetCallbackHook(void* channel, void* callback);

    void setOriginalChannelSetCallback(void* fn) { m_origChannelSetCallback = fn; }
    static void* getChannelSetCallbackHook();
};

} // namespace krkrspeed
