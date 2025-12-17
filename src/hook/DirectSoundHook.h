#pragma once

#include <windows.h>
#include <dsound.h>
#include <map>
#include <atomic>
#include <mutex>
#include <set>
#include <unordered_map>
#include <chrono>
#include "../common/DspPipeline.h"
#include "../common/AudioStreamProcessor.h"
#include "../common/VoiceContext.h"

namespace krkrspeed {

class DirectSoundHook {
public:
    static DirectSoundHook &instance();
    void initialize();
    struct Config {
        bool skip = false;
        bool disableBgm = false;
        bool processAllAudio = false;
        float bgmGateSeconds = 60.0f;
        std::uint32_t stereoBgmMode = 1; // 0 aggressive,1 hybrid(default),2 none
    };
    void configure(const Config &cfg);

    void applySharedSettingsFallback();

    // Allow late binding when DirectSoundCreate8 is resolved dynamically.
    void setOriginalCreate8(void *fn);
    void setOriginalCreate(void *fn);
    void scanLoadedModules();
    void bootstrapVtable();
    bool hasCreateHook() const { return m_origCreate8 != nullptr; }

    void patchDeviceVtable(IDirectSound8 *ds8);
    void patchBufferVtable(IDirectSoundBuffer *buf);
    void installGlobalUnlockHook();

    static HRESULT WINAPI DirectSoundCreate8Hook(LPCGUID pcGuidDevice, LPDIRECTSOUND8 *ppDS8, LPUNKNOWN pUnkOuter);
    static HRESULT WINAPI DirectSoundCreateHook(LPCGUID pcGuidDevice, LPDIRECTSOUND *ppDS, LPUNKNOWN pUnkOuter);
    static HRESULT WINAPI CreateSoundBufferHook(IDirectSound8 *self, LPDIRECTSOUNDBUFFER *ppDSBuffer,
                                                LPCDSBUFFERDESC pcDSBufferDesc);
    static HRESULT WINAPI UnlockHook(IDirectSoundBuffer *self, LPVOID pAudioPtr1, DWORD dwAudioBytes1, LPVOID pAudioPtr2,
                                     DWORD dwAudioBytes2);
    static ULONG __stdcall ReleaseHook(IDirectSoundBuffer *self);

    __declspec(noinline) HRESULT handleUnlock(IDirectSoundBuffer *self, LPVOID pAudioPtr1, DWORD dwAudioBytes1,
                                              LPVOID pAudioPtr2, DWORD dwAudioBytes2);

private:
    DirectSoundHook() = default;
    void hookEntryPoints();

    using PFN_DirectSoundCreate8 = HRESULT(WINAPI *)(LPCGUID, LPDIRECTSOUND8 *, LPUNKNOWN);
    using PFN_DirectSoundCreate = HRESULT(WINAPI *)(LPCGUID, LPDIRECTSOUND *, LPUNKNOWN);
    using PFN_CreateSoundBuffer = HRESULT(__stdcall *)(IDirectSound8 *, LPDIRECTSOUNDBUFFER *, LPCDSBUFFERDESC);
    using PFN_Unlock = HRESULT(__stdcall *)(IDirectSoundBuffer *, LPVOID, DWORD, LPVOID, DWORD);
    using PFN_Release = ULONG(__stdcall *)(IDirectSoundBuffer *);

    PFN_DirectSoundCreate8 m_origCreate8 = nullptr;
    PFN_DirectSoundCreate m_origCreate = nullptr;
    PFN_CreateSoundBuffer m_origCreateBuffer = nullptr;
    PFN_Unlock m_origUnlock = nullptr;
    PFN_Release m_origRelease = nullptr;

    struct BufferInfo {
        std::uint32_t sampleRate = 0;
        std::uint32_t channels = 0;
        std::uint16_t bitsPerSample = 16;
        bool isPcm16 = true;
        std::uint16_t formatTag = WAVE_FORMAT_PCM;
        std::uint32_t baseFrequency = 0;
        std::uint32_t bufferBytes = 0;
        std::uint32_t blockAlign = 0;
        float approxSeconds = 0.0f;
        bool isLikelyBgm = false;
        bool loggedFormat = false;
        std::unique_ptr<DspPipeline> dsp;
        std::uint64_t unlockCount = 0;
        std::uint64_t processedFrames = 0;
        DWORD currentFrequency = 0;
        bool freqDirty = false;
        std::unique_ptr<AudioStreamProcessor> stream;
    };
    std::map<std::uintptr_t, BufferInfo> m_buffers;
    std::set<std::string> m_loggedFormats;
    std::mutex m_mutex;
    std::mutex m_vtableMutex;
    std::unordered_map<void *, std::vector<void *>> m_deviceVtables;
    std::unordered_map<void *, std::vector<void *>> m_bufferVtables;
    std::atomic<bool> m_loggedUnlockOnce{false};
    std::atomic<bool> m_disableAfterFault{false};
    bool m_disableBgm = false;
    bool m_forceApply = false;
    float m_bgmSecondsGate = 60.0f;
    Config m_config{};
    std::atomic<bool> m_seenMono{false};
    std::atomic<bool> m_seenStereo{false};
    std::atomic<bool> m_fragmented{true};
    std::atomic<bool> m_loggedFragmentedClear{false};
    std::atomic<bool> m_loggedMonoStereo{false};
    std::unordered_map<std::uintptr_t, std::chrono::steady_clock::time_point> m_bgmReleaseTimes;
};

} // namespace krkrspeed
