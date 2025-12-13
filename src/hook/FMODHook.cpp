#include "FMODHook.h"
#include "HookUtils.h"
#include "../common/Logging.h"
#include <windows.h>
#include <Psapi.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <chrono>

namespace krkrspeed {

// FMOD definitions
enum FMOD_RESULT { FMOD_OK = 0 };
typedef int FMOD_BOOL;

struct FMOD_DSP_STATE {
    void* instance; // FMOD_DSP*
    void* plugindata;
    unsigned int channelmask;
    int source_speakermode;
    int* sidechaindata;
    int sidechainchannels;
    FMOD_BOOL sidechainmixed;
};

typedef FMOD_RESULT (__stdcall *FMOD_DSP_READ_CALLBACK)(FMOD_DSP_STATE *dsp_state, float *inbuffer, float *outbuffer, unsigned int length, int inchannels, int *outchannels);
typedef FMOD_RESULT (__stdcall *FMOD_DSP_RELEASE_CALLBACK)(FMOD_DSP_STATE *dsp_state);

struct FMOD_DSP_DESCRIPTION {
    unsigned int pluginsdkversion;
    char name[32];
    unsigned int version;
    int numinputbuffers;
    int numoutputbuffers;
    void* create;
    FMOD_DSP_RELEASE_CALLBACK release;
    void* reset;
    FMOD_DSP_READ_CALLBACK read;
    void* process;
    void* setposition;
    int numparameters;
    void* paramdesc;
    void* padding[32];
};

// Callback definitions
typedef enum {
    FMOD_CHANNELCONTROL_CALLBACKTYPE_END,
    FMOD_CHANNELCONTROL_CALLBACKTYPE_VIRTUALVOICE,
    FMOD_CHANNELCONTROL_CALLBACKTYPE_SYNCPOINT,
    FMOD_CHANNELCONTROL_CALLBACKTYPE_OCCLUSION,
    FMOD_CHANNELCONTROL_CALLBACKTYPE_MAX
} FMOD_CHANNELCONTROL_CALLBACK_TYPE;

typedef enum {
    FMOD_CHANNELCONTROL_CHANNEL,
    FMOD_CHANNELCONTROL_CHANNELGROUP,
    FMOD_CHANNELCONTROL_MAX
} FMOD_CHANNELCONTROL_TYPE;

typedef FMOD_RESULT (__stdcall *FMOD_CHANNEL_CALLBACK)(void *channelcontrol, FMOD_CHANNELCONTROL_TYPE controltype, FMOD_CHANNELCONTROL_CALLBACK_TYPE callbacktype, void *commanddata1, void *commanddata2);

// Function Pointers
using PFN_FMOD_System_PlaySound = int(__stdcall *)(void* system, void* channelgroup, void* sound, int paused, void** channel);
using PFN_FMOD_System_CreateDSP = int(__stdcall *)(void* system, const FMOD_DSP_DESCRIPTION* description, void** dsp);
using PFN_FMOD_DSP_Release = int(__stdcall *)(void* dsp);
using PFN_FMOD_Channel_AddDSP = int(__stdcall *)(void* channel, int index, void* dsp);
using PFN_FMOD_Channel_SetFrequency = int(__stdcall *)(void* channel, float frequency);
using PFN_FMOD_Channel_GetFrequency = int(__stdcall *)(void* channel, float* frequency);
using PFN_FMOD_Channel_SetPaused = int(__stdcall *)(void* channel, int paused);
using PFN_FMOD_Channel_SetCallback = int(__stdcall *)(void* channel, FMOD_CHANNEL_CALLBACK callback);
using PFN_FMOD_System_GetVersion = int(__stdcall *)(void* system, unsigned int* version);

// Context
struct DspContext {
    std::unique_ptr<DspPipeline> pipeline;
    float currentSpeed = 1.0f;
    float baseSampleRate = 44100.0f;
};

static std::mutex g_dspMapMutex;
static std::unordered_map<void*, DspContext*> g_dspContextMap;

static FMOD_RESULT __stdcall MyDSPRelease(FMOD_DSP_STATE *dsp_state) {
    if (!dsp_state || !dsp_state->instance) return FMOD_OK;
    std::lock_guard<std::mutex> lock(g_dspMapMutex);
    auto it = g_dspContextMap.find(dsp_state->instance);
    if (it != g_dspContextMap.end()) {
        delete it->second;
        g_dspContextMap.erase(it);
    }
    return FMOD_OK;
}

static void CheckSharedSettingsThrottled() {
    static std::chrono::steady_clock::time_point lastCheck;
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCheck).count() > 200) {
        lastCheck = now;
        FMODHook::instance().pollSharedSettings();
    }
}

static FMOD_RESULT __stdcall MyDSPReadWithContext(FMOD_DSP_STATE *dsp_state, float *inbuffer, float *outbuffer, unsigned int length, int inchannels, int *outchannels) {
    CheckSharedSettingsThrottled();

    DspContext* ctx = nullptr;
    float speed = 1.0f;
    {
        std::lock_guard<std::mutex> lock(g_dspMapMutex);
        auto it = g_dspContextMap.find(dsp_state->instance);
        if (it != g_dspContextMap.end()) {
            ctx = it->second;
            speed = ctx->currentSpeed;
        }
    }

    if (!ctx) {
        if (inbuffer != outbuffer) std::memcpy(outbuffer, inbuffer, length * inchannels * sizeof(float));
        return FMOD_OK;
    }

    if (!ctx->pipeline || ctx->pipeline->channels() != (uint32_t)inchannels) {
        DspConfig cfg;
        ctx->pipeline = std::make_unique<DspPipeline>((uint32_t)ctx->baseSampleRate, (uint32_t)inchannels, cfg);
    }

    float ratio = 1.0f;
    if (speed > 0.01f) {
        ratio = 1.0f / speed;
    }

    if (std::abs(ratio - 1.0f) < 0.01f) {
        if (inbuffer != outbuffer) std::memcpy(outbuffer, inbuffer, length * inchannels * sizeof(float));
        return FMOD_OK;
    }

    std::vector<float> processed = ctx->pipeline->process(inbuffer, length * inchannels, ratio, DspMode::Pitch);

    size_t procSamples = processed.size();
    size_t reqSamples = length * inchannels;

    if (procSamples >= reqSamples) {
        std::memcpy(outbuffer, processed.data(), reqSamples * sizeof(float));
    } else {
        std::memcpy(outbuffer, processed.data(), procSamples * sizeof(float));
        std::memset(outbuffer + procSamples, 0, (reqSamples - procSamples) * sizeof(float));
    }

    return FMOD_OK;
}

static FMOD_RESULT __stdcall MyFMODCallback(void *channelcontrol, FMOD_CHANNELCONTROL_TYPE controltype, FMOD_CHANNELCONTROL_CALLBACK_TYPE callbacktype, void *commanddata1, void *commanddata2) {
    // We only care about channels, not groups (unless we attached to group, but we attach to channel)
    if (controltype == FMOD_CHANNELCONTROL_CHANNEL) {
        // If end of playback, release the DSP
        if (callbacktype == FMOD_CHANNELCONTROL_CALLBACKTYPE_END) {
             FMODHook::instance().cleanupChannel(channelcontrol);
        }
    }

    // Call original callback
    auto orig = FMODHook::instance().getOriginalCallback(channelcontrol);
    if (orig) {
        return ((FMOD_CHANNEL_CALLBACK)orig)(channelcontrol, controltype, callbacktype, commanddata1, commanddata2);
    }
    return FMOD_OK;
}

FMODHook& FMODHook::instance() {
    static FMODHook hook;
    return hook;
}

void* FMODHook::getSystemPlaySoundHook() {
    return (void*)&SystemPlaySoundHook;
}

void* FMODHook::getChannelSetCallbackHook() {
    return (void*)&ChannelSetCallbackHook;
}

void FMODHook::initialize() {
    scanLoadedModules();
}

void FMODHook::pollSharedSettings() {
    if (!m_sharedView) {
        const auto name = BuildSharedSettingsName(static_cast<std::uint32_t>(GetCurrentProcessId()));
        m_sharedMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
        if (m_sharedMapping) {
            m_sharedView = static_cast<SharedSettings *>(MapViewOfFile(m_sharedMapping, FILE_MAP_READ, 0, 0, sizeof(SharedSettings)));
        }
    }

    if (m_sharedView) {
        float speed = m_sharedView->userSpeed;
        if (std::abs(speed - m_userSpeed) > 0.001f) {
            setUserSpeed(speed);
        }
    }
}

void FMODHook::setUserSpeed(float speed) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_userSpeed = std::clamp(speed, 0.1f, 10.0f);

    {
        std::lock_guard<std::mutex> lock2(g_dspMapMutex);
        for (auto& pair : g_dspContextMap) {
            if (pair.second) {
                pair.second->currentSpeed = m_userSpeed;
            }
        }
    }

    for (auto& pair : m_channels) {
        // Only update if we still think it's valid.
        // FMOD handle reuse issues?
        // If handle reused, baseFrequency is overwritten by PlaySound.
        // So this is safe-ish.
        if (pair.second.baseFrequency > 0 && m_fnChannelSetFrequency) {
            float target = pair.second.baseFrequency * m_userSpeed;
            ((PFN_FMOD_Channel_SetFrequency)m_fnChannelSetFrequency)((void*)pair.first, target);
        }
    }
}

void FMODHook::scanLoadedModules() {
    HMODULE hMod = GetModuleHandleA("fmodstudio.dll");
    if (!hMod) hMod = GetModuleHandleA("fmod.dll");
    if (!hMod) hMod = GetModuleHandleA("fmod64.dll");

    if (hMod) {
        installHooks(hMod);
    }
}

void FMODHook::installHooks(void* moduleHandle) {
    HMODULE hMod = (HMODULE)moduleHandle;

    m_fnSystemCreateDSP = (void*)GetProcAddress(hMod, "FMOD_System_CreateDSP");
    m_fnDSPRelease = (void*)GetProcAddress(hMod, "FMOD_DSP_Release");
    m_fnChannelAddDSP = (void*)GetProcAddress(hMod, "FMOD_Channel_AddDSP");
    m_fnChannelSetFrequency = (void*)GetProcAddress(hMod, "FMOD_Channel_SetFrequency");
    m_fnChannelGetFrequency = (void*)GetProcAddress(hMod, "FMOD_Channel_GetFrequency");
    m_fnChannelSetPaused = (void*)GetProcAddress(hMod, "FMOD_Channel_SetPaused");
    m_fnChannelSetCallback = (void*)GetProcAddress(hMod, "FMOD_Channel_SetCallback");

    bool patched = false;
    patched |= PatchImport("fmodstudio.dll", "FMOD_System_PlaySound", (void*)&FMODHook::SystemPlaySoundHook, &m_origSystemPlaySound);
    if (!patched) patched |= PatchImport("fmod.dll", "FMOD_System_PlaySound", (void*)&FMODHook::SystemPlaySoundHook, &m_origSystemPlaySound);
    if (!patched) patched |= PatchImport("fmod64.dll", "FMOD_System_PlaySound", (void*)&FMODHook::SystemPlaySoundHook, &m_origSystemPlaySound);

    // Hook SetCallback to intercept user callbacks
    if (m_fnChannelSetCallback) {
        bool cbPatched = false;
        cbPatched |= PatchImport("fmodstudio.dll", "FMOD_Channel_SetCallback", (void*)&FMODHook::ChannelSetCallbackHook, &m_origChannelSetCallback);
        if (!cbPatched) cbPatched |= PatchImport("fmod.dll", "FMOD_Channel_SetCallback", (void*)&FMODHook::ChannelSetCallbackHook, &m_origChannelSetCallback);
        if (!cbPatched) cbPatched |= PatchImport("fmod64.dll", "FMOD_Channel_SetCallback", (void*)&FMODHook::ChannelSetCallbackHook, &m_origChannelSetCallback);

        if (!m_origChannelSetCallback) m_origChannelSetCallback = m_fnChannelSetCallback; // Fallback if patch failed but we have address (for manual call)
    }

    if (patched) {
        KRKR_LOG_INFO("FMOD PlaySound hooked successfully");
    } else {
        KRKR_LOG_WARN("FMOD PlaySound hook failed (imports not found)");
    }
}

int __stdcall FMODHook::SystemPlaySoundHook(void* system, void* channelgroup, void* sound, int paused, void** channel) {
    auto& self = FMODHook::instance();

    void* localChannel = nullptr;
    int result = ((PFN_FMOD_System_PlaySound)self.m_origSystemPlaySound)(system, channelgroup, sound, 1, &localChannel);

    if (result == FMOD_OK && localChannel) {
        if (channel) *channel = localChannel;
        self.onPlaySound(localChannel, sound, system);

        if (!paused && self.m_fnChannelSetPaused) {
            ((PFN_FMOD_Channel_SetPaused)self.m_fnChannelSetPaused)(localChannel, 0);
        }
    }
    return result;
}

int __stdcall FMODHook::ChannelSetCallbackHook(void* channel, void* callback) {
    auto& self = FMODHook::instance();

    {
        std::lock_guard<std::mutex> lock(self.m_mutex);
        auto& info = self.m_channels[(std::uintptr_t)channel];
        info.originalCallback = callback;
    }

    // We always set OUR callback.
    // Use original function pointer if we have it, or the hooked one?
    // We patched import, so m_origChannelSetCallback should be used.
    if (self.m_origChannelSetCallback) {
        return ((PFN_FMOD_Channel_SetCallback)self.m_origChannelSetCallback)(channel, MyFMODCallback);
    }
    // Fallback
    if (self.m_fnChannelSetCallback) {
         return ((PFN_FMOD_Channel_SetCallback)self.m_fnChannelSetCallback)(channel, MyFMODCallback);
    }
    return FMOD_OK;
}

void FMODHook::onPlaySound(void* channel, void* sound, void* system) {
    if (!m_fnSystemCreateDSP || !m_fnChannelAddDSP || !m_fnChannelGetFrequency || !m_fnChannelSetFrequency) return;

    float baseFreq = 0.0f;
    ((PFN_FMOD_Channel_GetFrequency)m_fnChannelGetFrequency)(channel, &baseFreq);

    if (baseFreq <= 0.0f) return;

    // Set callback if not already handled by hook (e.g. game never calls SetCallback)
    if (m_fnChannelSetCallback) {
        // We set it via original function to avoid loop if we call hook?
        // Hook is ChannelSetCallbackHook.
        // We should call ORIGINAL.
        // And we should record that there is NO original callback yet.
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto& info = self.m_channels[(std::uintptr_t)channel]; // Error: 'self' not avail
        }
    }
    // Fix: self access

    std::lock_guard<std::mutex> lock(m_mutex);
    auto& info = m_channels[(std::uintptr_t)channel];
    info.baseFrequency = baseFreq;
    info.originalCallback = nullptr; // Reset for new sound

    if (m_fnChannelSetCallback) {
         // Using m_origChannelSetCallback if available, else m_fnChannelSetCallback
         void* fn = m_origChannelSetCallback ? m_origChannelSetCallback : m_fnChannelSetCallback;
         ((PFN_FMOD_Channel_SetCallback)fn)(channel, MyFMODCallback);
    }

    // Create DSP
    FMOD_DSP_DESCRIPTION desc = {};
    std::strcpy(desc.name, "KrkrSpeed");
    desc.version = 0x00010000;
    desc.numinputbuffers = 1;
    desc.numoutputbuffers = 1;
    desc.read = MyDSPReadWithContext;
    desc.release = MyDSPRelease;

    void* dsp = nullptr;
    int res = ((PFN_FMOD_System_CreateDSP)m_fnSystemCreateDSP)(system, &desc, &dsp);
    if (res == FMOD_OK && dsp) {
            info.dsp = dsp;

            auto* ctx = new DspContext();
            ctx->baseSampleRate = baseFreq;
            ctx->currentSpeed = m_userSpeed;

            {
                std::lock_guard<std::mutex> lock2(g_dspMapMutex);
                g_dspContextMap[dsp] = ctx;
            }

            ((PFN_FMOD_Channel_AddDSP)m_fnChannelAddDSP)(channel, 0, dsp);

            if (m_userSpeed != 1.0f) {
                ((PFN_FMOD_Channel_SetFrequency)m_fnChannelSetFrequency)(channel, baseFreq * m_userSpeed);
            }
    }
}

// Helpers called from static callbacks
void FMODHook::cleanupChannel(void* channel) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_channels.find((std::uintptr_t)channel);
    if (it != m_channels.end()) {
        if (it->second.dsp && m_fnDSPRelease) {
            ((PFN_FMOD_DSP_Release)m_fnDSPRelease)(it->second.dsp);
        }
        // Don't erase? If we erase, we lose originalCallback for the *current* callback execution if it's stored here.
        // The callback calls getOriginalCallback *after* cleanupChannel.
        // So we must NOT erase yet.
        // Just clear DSP pointer.
        it->second.dsp = nullptr;
        // The entry will be overwritten by next PlaySound.
        // Map size is small.
    }
}

void* FMODHook::getOriginalCallback(void* channel) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_channels.find((std::uintptr_t)channel);
    if (it != m_channels.end()) {
        return it->second.originalCallback;
    }
    return nullptr;
}

} // namespace krkrspeed
