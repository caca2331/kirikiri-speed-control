#pragma once

#include "../common/DspPipeline.h"
#include "../common/VoiceContext.h"
#include "../common/SharedSettings.h"
#include <cstdint>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>
#include <psapi.h>
#include <xaudio2.h>

namespace krkrspeed {

class XAudio2Hook {
public:
    static XAudio2Hook &instance();
    void initialize();

    void setUserSpeed(float speed);
    void configureLengthGate(bool enabled, float seconds);
    float getUserSpeed() const { return m_userSpeed; }
    bool isLengthGateEnabled() const { return m_lengthGateEnabled; }
    float lengthGateSeconds() const { return m_lengthGateSeconds; }
    void pollSharedSettings();

    // Allow late binding when targets resolve XAudio2Create dynamically.
    void setOriginalCreate(void *fn);
    bool hasCreateHook() const { return m_origCreate != nullptr; }

    static HRESULT WINAPI XAudio2CreateHook(IXAudio2 **ppXAudio2, UINT32 Flags, XAUDIO2_PROCESSOR XAudio2Processor);
    static HRESULT WINAPI CoCreateInstanceHook(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext, REFIID riid,
                                               LPVOID *ppv);
    static HRESULT __stdcall CreateSourceVoiceHook(IXAudio2 *self, IXAudio2SourceVoice **ppSourceVoice,
                                                   const WAVEFORMATEX *fmt, UINT32 Flags, float MaxFreqRatio,
                                                   IXAudio2VoiceCallback *cb, const XAUDIO2_EFFECT_CHAIN *chain,
                                                   const XAUDIO2_FILTER_PARAMETERS *filter);
    static HRESULT __stdcall SubmitSourceBufferHook(IXAudio2SourceVoice *voice, const XAUDIO2_BUFFER *pBuffer,
                                                    const XAUDIO2_BUFFER_WMA *pBufferWMA);
    static HRESULT __stdcall SetFrequencyRatioHook(IXAudio2SourceVoice *voice, float ratio, UINT32 operationSet);
    static void __stdcall DestroyVoiceHook(IXAudio2Voice *voice);

private:
    XAudio2Hook() = default;
    void detectVersion();
    void hookEntryPoints();
    void ensureCreateFunction();
    void bootstrapVtable();
    void scheduleBootstrapRetries();
    void scanLoadedModules();
    void onCreateSourceVoice(std::uintptr_t voiceKey, std::uint32_t sampleRate, std::uint32_t channels);
    std::vector<std::uint8_t> onSubmitBuffer(std::uintptr_t voiceKey, const std::uint8_t *data, std::size_t size);
    void attachSharedSettings();
    void applySharedSettingsLocked(const SharedSettings &settings);

    float m_userSpeed = 2.0f;
    bool m_lengthGateEnabled = true;
    float m_lengthGateSeconds = 30.0f;
    std::map<std::uintptr_t, VoiceContext> m_contexts;
    std::mutex m_mutex;
    std::string m_version;
    HANDLE m_sharedMapping = nullptr;
    SharedSettings *m_sharedView = nullptr;

    // Original functions.
    using PFN_XAudio2Create = HRESULT(WINAPI *)(IXAudio2 **ppXAudio2, UINT32 Flags, XAUDIO2_PROCESSOR XAudio2Processor);
    using PFN_CoCreateInstance = HRESULT(WINAPI *)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
    PFN_XAudio2Create m_origCreate = nullptr;
    PFN_CoCreateInstance m_origCoCreate = nullptr;

    using PFN_CreateSourceVoice = HRESULT(__stdcall *)(IXAudio2 *, IXAudio2SourceVoice **, const WAVEFORMATEX *,
                                                       UINT32, float, IXAudio2VoiceCallback *,
                                                       const XAUDIO2_EFFECT_CHAIN *, const XAUDIO2_FILTER_PARAMETERS *);
    using PFN_SubmitSourceBuffer = HRESULT(__stdcall *)(IXAudio2SourceVoice *, const XAUDIO2_BUFFER *,
                                                        const XAUDIO2_BUFFER_WMA *);
    using PFN_SetFrequencyRatio = HRESULT(__stdcall *)(IXAudio2SourceVoice *, float, UINT32);
    using PFN_DestroyVoice = void(__stdcall *)(IXAudio2Voice *);

    PFN_CreateSourceVoice m_origCreateSourceVoice = nullptr;
    PFN_SubmitSourceBuffer m_origSubmit = nullptr;
    PFN_SetFrequencyRatio m_origSetFreq = nullptr;
    PFN_DestroyVoice m_origDestroyVoice = nullptr;

    std::atomic<bool> m_loggedSubmitOnce{false};
};

} // namespace krkrspeed
