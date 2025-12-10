#include "XAudio2Hook.h"
#include "HookUtils.h"
#include "../common/Logging.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <deque>
#include <vector>
#include <cmath>
#include <thread>
#include <chrono>

namespace krkrspeed {

namespace {
XAudio2Hook *g_self = nullptr;
// XAudio2 2.7 COM CLSIDs (from xaudio2_7 GUIDs)
const GUID kClsidXAudio2_27 = {0x5a508685, 0xa254, 0x4fba, {0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf}};
const GUID kClsidXAudio2Debug_27 = {0xe21fef06, 0x8c6b, 0x4e0a, {0x9a, 0x22, 0x0e, 0x0d, 0xe0, 0xf9, 0xf7, 0xe8}};
}

XAudio2Hook &XAudio2Hook::instance() {
    static XAudio2Hook hook;
    return hook;
}

void XAudio2Hook::initialize() {
    g_self = this;
    // Default gate: always on, 60s or overridden by env KRKR_DS_BGM_SECS for consistency.
    auto envFloat = [](const wchar_t *name, float fallback) {
        wchar_t buf[32] = {};
        DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
        if (n == 0 || n >= std::size(buf)) return fallback;
        try { return std::stof(std::wstring(buf)); } catch (...) { return fallback; }
    };
    m_lengthGateEnabled = true;
    m_lengthGateSeconds = envFloat(L"KRKR_DS_BGM_SECS", 60.0f);
    detectVersion();
    hookEntryPoints();
    ensureCreateFunction();
    scanLoadedModules();
    bootstrapVtable();
    scheduleBootstrapRetries();
    attachSharedSettings();
    pollSharedSettings();
    KRKR_LOG_INFO("XAudio2 hook initialized for version " + m_version);
}

void XAudio2Hook::detectVersion() {
    HMODULE xa25 = GetModuleHandleA("XAudio2_5.dll");
    HMODULE xa26 = GetModuleHandleA("XAudio2_6.dll");
    HMODULE xa27 = GetModuleHandleA("XAudio2_7.dll");
    HMODULE xa28 = GetModuleHandleA("XAudio2_8.dll");
    HMODULE xa29 = GetModuleHandleA("XAudio2_9.dll");
    if (xa29) m_version = "2.9";
    else if (xa28) m_version = "2.8";
    else if (xa27) m_version = "2.7";
    else if (xa26) m_version = "2.6";
    else if (xa25) m_version = "2.5";
    else m_version = "unknown";
    KRKR_LOG_DEBUG("Detected XAudio2 version: " + m_version);
}

void XAudio2Hook::hookEntryPoints() {
    bool patched = false;
    if (PatchImport("XAudio2_9.dll", "XAudio2Create", reinterpret_cast<void *>(&XAudio2Hook::XAudio2CreateHook),
                    reinterpret_cast<void **>(&m_origCreate))) {
        m_version = "2.9";
        patched = true;
    } else if (PatchImport("XAudio2_8.dll", "XAudio2Create", reinterpret_cast<void *>(&XAudio2Hook::XAudio2CreateHook),
                           reinterpret_cast<void **>(&m_origCreate))) {
        m_version = "2.8";
        patched = true;
    } else if (PatchImport("XAudio2_7.dll", "XAudio2Create", reinterpret_cast<void *>(&XAudio2Hook::XAudio2CreateHook),
                           reinterpret_cast<void **>(&m_origCreate))) {
        m_version = "2.7";
        patched = true;
    } else if (PatchImport("XAudio2_6.dll", "XAudio2Create", reinterpret_cast<void *>(&XAudio2Hook::XAudio2CreateHook),
                           reinterpret_cast<void **>(&m_origCreate))) {
        m_version = "2.6";
        patched = true;
    } else if (PatchImport("XAudio2_5.dll", "XAudio2Create", reinterpret_cast<void *>(&XAudio2Hook::XAudio2CreateHook),
                           reinterpret_cast<void **>(&m_origCreate))) {
        m_version = "2.5";
        patched = true;
    }
    if (!patched) {
        // XAudio2_7 often uses COM activation.
        if (PatchImport("ole32.dll", "CoCreateInstance", reinterpret_cast<void *>(&XAudio2Hook::CoCreateInstanceHook),
                        reinterpret_cast<void **>(&m_origCoCreate))) {
            m_version = "2.7";
            patched = true;
            KRKR_LOG_INFO("Patched CoCreateInstance import for XAudio2_7 detection");
        }
    }

    if (patched) {
        KRKR_LOG_INFO("Patched XAudio2Create import for version " + m_version);
    } else {
        KRKR_LOG_WARN("Failed to patch XAudio2Create import; will fall back to GetProcAddress interception");
    }
}

void XAudio2Hook::ensureCreateFunction() {
    if (m_origCreate) {
        return;
    }
    HMODULE xa9 = GetModuleHandleA("XAudio2_9.dll");
    HMODULE xa8 = GetModuleHandleA("XAudio2_8.dll");
    HMODULE xa7 = GetModuleHandleA("XAudio2_7.dll");
    HMODULE xa6 = GetModuleHandleA("XAudio2_6.dll");
    HMODULE xa5 = GetModuleHandleA("XAudio2_5.dll");
    HMODULE chosen = xa9 ? xa9 : (xa8 ? xa8 : (xa7 ? xa7 : (xa6 ? xa6 : xa5)));
    if (!chosen) {
        chosen = LoadLibraryA("XAudio2_7.dll");
    }
    if (!chosen) {
        chosen = LoadLibraryA("XAudio2_6.dll");
    }
    if (!chosen) {
        chosen = LoadLibraryA("XAudio2_5.dll");
    }
    if (!chosen) {
        KRKR_LOG_WARN("Could not load any XAudio2 DLL for manual lookup");
        return;
    }
    auto fn = reinterpret_cast<PFN_XAudio2Create>(GetProcAddress(chosen, "XAudio2Create"));
    if (fn) {
        m_origCreate = fn;
        if (chosen == xa9) m_version = "2.9";
        else if (chosen == xa8) m_version = "2.8";
        else m_version = "2.7";
        KRKR_LOG_INFO("Captured XAudio2Create via manual lookup for version " + m_version);
    } else {
        KRKR_LOG_WARN("XAudio2Create export not found in loaded DLL; will rely on COM bootstrap");
    }
}

void XAudio2Hook::scanLoadedModules() {
    HMODULE modules[256];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
        return;
    }
    const size_t count = std::min<std::size_t>(needed / sizeof(HMODULE), std::size(modules));
    for (size_t i = 0; i < count; ++i) {
        char name[MAX_PATH] = {};
        if (GetModuleBaseNameA(GetCurrentProcess(), modules[i], name, static_cast<DWORD>(std::size(name))) == 0) {
            continue;
        }
        std::string lower(name);
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find("xaudio2") != std::string::npos) {
            FARPROC fn = GetProcAddress(modules[i], "XAudio2Create");
            if (fn) {
                setOriginalCreate(reinterpret_cast<void *>(fn));
                KRKR_LOG_INFO(std::string("scanLoadedModules captured XAudio2Create from ") + name);
            }
        }
    }
}

void XAudio2Hook::bootstrapVtable() {
    IXAudio2 *xa = nullptr;
    HRESULT hr = E_FAIL;
    if (m_origCreate) {
        hr = m_origCreate(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
    }
    if (FAILED(hr) || !xa) {
        // Try COM activation (XAudio2_7 style) as a fallback.
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        hr = CoCreateInstance(kClsidXAudio2_27, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IXAudio2),
                              reinterpret_cast<void **>(&xa));
        if (FAILED(hr) || !xa) {
            KRKR_LOG_WARN("Bootstrap could not create IXAudio2 instance (hr=" + std::to_string(hr) + ")");
            return;
        }
        m_version = "2.7";
    }
    void **vtbl = *reinterpret_cast<void ***>(xa);
    bool patched = false;
    if (!m_origCreateSourceVoice) {
        patched |= PatchVtableEntry(vtbl, 8, &XAudio2Hook::CreateSourceVoiceHook, m_origCreateSourceVoice);
    }
    if (!m_origSubmit) {
        patched |= PatchVtableEntry(vtbl, 21, &XAudio2Hook::SubmitSourceBufferHook, m_origSubmit);
    }
    if (!m_origSetFreq) {
        patched |= PatchVtableEntry(vtbl, 26, &XAudio2Hook::SetFrequencyRatioHook, m_origSetFreq);
    }
    if (!m_origDestroyVoice) {
        patched |= PatchVtableEntry(vtbl, 18, &XAudio2Hook::DestroyVoiceHook, m_origDestroyVoice);
    }
    xa->Release();
    if (patched) {
        KRKR_LOG_INFO("Bootstrapped IXAudio2 vtable patch via self-created instance");
    } else {
        KRKR_LOG_DEBUG("Bootstrap found vtable already patched");
    }
}

void XAudio2Hook::scheduleBootstrapRetries() {
    std::thread([] {
        for (int i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            auto &self = XAudio2Hook::instance();
            {
                std::lock_guard<std::mutex> lock(self.m_mutex);
                if (self.m_origSubmit) {
                    return;
                }
            }
            self.ensureCreateFunction();
            self.bootstrapVtable();
        }
        KRKR_LOG_WARN("Bootstrap retries exhausted without obtaining XAudio2 vtable; audio may remain unhooked");
    }).detach();
}

void XAudio2Hook::attachSharedSettings() {
    const auto name = BuildSharedSettingsName(static_cast<std::uint32_t>(GetCurrentProcessId()));
    m_sharedMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!m_sharedMapping) {
        KRKR_LOG_WARN("Shared settings map not found; using defaults");
        return;
    }
    m_sharedView = static_cast<SharedSettings *>(MapViewOfFile(m_sharedMapping, FILE_MAP_READ, 0, 0, sizeof(SharedSettings)));
    if (!m_sharedView) {
        KRKR_LOG_WARN("MapViewOfFile failed for shared settings");
        CloseHandle(m_sharedMapping);
        m_sharedMapping = nullptr;
        return;
    }
    KRKR_LOG_INFO("Attached to shared settings map");
}

void XAudio2Hook::applySharedSettingsLocked(const SharedSettings &settings) {
    const float newSpeed = std::clamp(settings.userSpeed, 0.5f, 10.0f);
    const float newGateSeconds = std::clamp(settings.lengthGateSeconds, 0.1f, 600.0f);
    const bool gateEnabled = settings.lengthGateEnabled != 0;
    const bool speedChanged = std::fabs(newSpeed - m_userSpeed) > 0.001f;
    const bool gateChanged =
        gateEnabled != m_lengthGateEnabled || std::fabs(newGateSeconds - m_lengthGateSeconds) > 0.001f;

    m_userSpeed = newSpeed;
    m_lengthGateEnabled = gateEnabled;
    m_lengthGateSeconds = newGateSeconds;

    if (speedChanged) {
        for (auto &kv : m_contexts) {
            kv.second.userSpeed = m_userSpeed;
            kv.second.effectiveSpeed = kv.second.userSpeed * kv.second.engineRatio;
        }
        KRKR_LOG_INFO("Shared speed updated to " + std::to_string(m_userSpeed) + "x");
    }
    if (gateChanged) {
        KRKR_LOG_INFO(std::string("Shared length gate ") + (m_lengthGateEnabled ? "enabled" : "disabled") + " @ " +
                      std::to_string(m_lengthGateSeconds) + "s");
    }
}

void XAudio2Hook::pollSharedSettings() {
    if (!m_sharedView) {
        attachSharedSettings();
        if (!m_sharedView) return;
    }
    SharedSettings snapshot = *m_sharedView;
    std::lock_guard<std::mutex> lock(m_mutex);
    applySharedSettingsLocked(snapshot);
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

void XAudio2Hook::setOriginalCreate(void *fn) {
    if (!fn || m_origCreate) {
        return;
    }
    m_origCreate = reinterpret_cast<PFN_XAudio2Create>(fn);
    KRKR_LOG_DEBUG("Captured XAudio2Create via GetProcAddress; enabling XAudio2 interception");
    if (!m_origSubmit) {
        bootstrapVtable();
    }
}

void XAudio2Hook::configureLengthGate(bool enabled, float seconds) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lengthGateEnabled = enabled;
    m_lengthGateSeconds = std::clamp(seconds, 0.1f, 600.0f);
    KRKR_LOG_INFO(std::string("Length gate ") + (enabled ? "enabled" : "disabled") + " at " +
                  std::to_string(m_lengthGateSeconds) + "s");
}

HRESULT WINAPI XAudio2Hook::CoCreateInstanceHook(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext, REFIID riid,
                                                 LPVOID *ppv) {
    if (!g_self || !g_self->m_origCoCreate) {
        return REGDB_E_CLASSNOTREG;
    }
    const bool isXAudioClsid = IsEqualCLSID(rclsid, kClsidXAudio2_27) || IsEqualCLSID(rclsid, kClsidXAudio2Debug_27);
    const HRESULT hr = g_self->m_origCoCreate(rclsid, pUnkOuter, dwClsContext, riid, ppv);
    if (FAILED(hr) || !ppv || !*ppv || !isXAudioClsid) {
        return hr;
    }

    g_self->m_version = "2.7";
    auto *xa2 = reinterpret_cast<IXAudio2 *>(*ppv);
    void **vtbl = *reinterpret_cast<void ***>(xa2);
    if (!g_self->m_origCreateSourceVoice) {
        PatchVtableEntry(vtbl, 8, &XAudio2Hook::CreateSourceVoiceHook, g_self->m_origCreateSourceVoice);
        KRKR_LOG_INFO("IXAudio2 vtable patched via CoCreateInstance (CreateSourceVoice)");
    }
    return hr;
}

HRESULT WINAPI XAudio2Hook::XAudio2CreateHook(IXAudio2 **ppXAudio2, UINT32 Flags, XAUDIO2_PROCESSOR proc) {
    if (!g_self || !g_self->m_origCreate) {
        return XAUDIO2_E_INVALID_CALL;
    }
    const HRESULT hr = g_self->m_origCreate(ppXAudio2, Flags, proc);
    if (FAILED(hr) || !ppXAudio2 || !*ppXAudio2) {
        return hr;
    }

    void **vtbl = *reinterpret_cast<void ***>(*ppXAudio2);
    if (!g_self->m_origCreateSourceVoice) {
        PatchVtableEntry(vtbl, 8, &XAudio2Hook::CreateSourceVoiceHook, g_self->m_origCreateSourceVoice);
        KRKR_LOG_INFO("IXAudio2 vtable patched (CreateSourceVoice)");
    }
    return hr;
}

HRESULT __stdcall XAudio2Hook::CreateSourceVoiceHook(IXAudio2 *self, IXAudio2SourceVoice **ppSourceVoice,
                                                     const WAVEFORMATEX *fmt, UINT32 Flags, float MaxFreqRatio,
                                                     IXAudio2VoiceCallback *cb, const XAUDIO2_EFFECT_CHAIN *chain,
                                                     const XAUDIO2_FILTER_PARAMETERS *filter) {
    auto &hook = XAudio2Hook::instance();
    if (!hook.m_origCreateSourceVoice) {
        return XAUDIO2_E_INVALID_CALL;
    }
    const HRESULT hr =
        hook.m_origCreateSourceVoice(self, ppSourceVoice, fmt, Flags, MaxFreqRatio, cb, chain, filter);
    if (FAILED(hr) || !ppSourceVoice || !*ppSourceVoice || !fmt) {
        return hr;
    }

    std::uintptr_t key = reinterpret_cast<std::uintptr_t>(*ppSourceVoice);
    hook.onCreateSourceVoice(key, fmt->nSamplesPerSec, fmt->nChannels);

    void **vtbl = *reinterpret_cast<void ***>(*ppSourceVoice);
    if (!hook.m_origSubmit) {
        PatchVtableEntry(vtbl, 21, &XAudio2Hook::SubmitSourceBufferHook, hook.m_origSubmit);
    }
    if (!hook.m_origSetFreq) {
        PatchVtableEntry(vtbl, 26, &XAudio2Hook::SetFrequencyRatioHook, hook.m_origSetFreq);
    }
    if (!hook.m_origDestroyVoice) {
        PatchVtableEntry(vtbl, 18, &XAudio2Hook::DestroyVoiceHook, hook.m_origDestroyVoice);
    }

    KRKR_LOG_DEBUG("Patched IXAudio2SourceVoice vtable entries");
    return hr;
}

void __stdcall XAudio2Hook::DestroyVoiceHook(IXAudio2Voice *voice) {
    auto &hook = XAudio2Hook::instance();
    {
        std::lock_guard<std::mutex> lock(hook.m_mutex);
        hook.m_contexts.erase(reinterpret_cast<std::uintptr_t>(voice));
    }
    if (hook.m_origDestroyVoice) {
        hook.m_origDestroyVoice(voice);
    }
}

HRESULT __stdcall XAudio2Hook::SetFrequencyRatioHook(IXAudio2SourceVoice *voice, float ratio, UINT32 operationSet) {
    auto &hook = XAudio2Hook::instance();
    if (hook.m_origSetFreq) {
        std::lock_guard<std::mutex> lock(hook.m_mutex);
        auto it = hook.m_contexts.find(reinterpret_cast<std::uintptr_t>(voice));
        if (it != hook.m_contexts.end()) {
            it->second.engineRatio = ratio;
            it->second.effectiveSpeed = it->second.userSpeed * it->second.engineRatio;
        }
        return hook.m_origSetFreq(voice, ratio, operationSet);
    }
    return XAUDIO2_E_INVALID_CALL;
}

HRESULT __stdcall XAudio2Hook::SubmitSourceBufferHook(IXAudio2SourceVoice *voice, const XAUDIO2_BUFFER *pBuffer,
                                                      const XAUDIO2_BUFFER_WMA *pBufferWMA) {
    auto &hook = XAudio2Hook::instance();
    if (!hook.m_origSubmit || !pBuffer || !pBuffer->pAudioData || pBuffer->AudioBytes == 0) {
        return XAUDIO2_E_INVALID_CALL;
    }
    static bool disableDsp = []{
        wchar_t buf[4] = {};
        return GetEnvironmentVariableW(L"KRKR_DISABLE_DSP", buf, static_cast<DWORD>(std::size(buf))) > 0;
    }();
    hook.pollSharedSettings();

    std::vector<std::uint8_t> processed;
    {
        std::lock_guard<std::mutex> lock(hook.m_mutex);
        auto it = hook.m_contexts.find(reinterpret_cast<std::uintptr_t>(voice));
        if (it == hook.m_contexts.end()) {
            return hook.m_origSubmit(voice, pBuffer, pBufferWMA);
        }
        if (!hook.m_loggedSubmitOnce.exchange(true)) {
            KRKR_LOG_INFO("SubmitSourceBufferHook engaged for voice=" + std::to_string(reinterpret_cast<std::uintptr_t>(voice)));
        }

        // Assume 16-bit PCM.
        if (!disableDsp) {
            processed = hook.onSubmitBuffer(reinterpret_cast<std::uintptr_t>(voice),
                                            reinterpret_cast<const std::uint8_t *>(pBuffer->pAudioData),
                                            pBuffer->AudioBytes);
        }
        if (processed.empty()) {
            return hook.m_origSubmit(voice, pBuffer, pBufferWMA);
        }
    }

    // Prepare a copy of the buffer with replaced data.
    XAUDIO2_BUFFER copy = *pBuffer;
    copy.pAudioData = processed.data();
    copy.AudioBytes = static_cast<UINT32>(processed.size());

    // Keep payload alive by storing in the context.
    {
        std::lock_guard<std::mutex> lock(hook.m_mutex);
        auto &ctx = hook.m_contexts[reinterpret_cast<std::uintptr_t>(voice)];
        BufferMeta meta;
        meta.payload = std::move(processed);
        ctx.pendingBuffers.push_back(std::move(meta));
        // Prevent unbounded growth.
        while (ctx.pendingBuffers.size() > 16) {
            ctx.pendingBuffers.pop_front();
        }
    }

    return hook.m_origSubmit(voice, &copy, pBufferWMA);
}

void XAudio2Hook::onCreateSourceVoice(std::uintptr_t voiceKey, std::uint32_t sampleRate, std::uint32_t channels) {
    std::lock_guard<std::mutex> lock(m_mutex);
    VoiceContext ctx;
    ctx.sampleRate = sampleRate;
    ctx.channels = channels;
    ctx.userSpeed = m_userSpeed;
    ctx.effectiveSpeed = ctx.userSpeed * ctx.engineRatio;
    m_contexts.emplace(voiceKey, std::move(ctx));
    KRKR_LOG_DEBUG("Created voice context key=" + std::to_string(voiceKey) + " sr=" + std::to_string(sampleRate) +
                   " ch=" + std::to_string(channels));
}

std::vector<std::uint8_t> XAudio2Hook::onSubmitBuffer(std::uintptr_t voiceKey, const std::uint8_t *data,
                                                      std::size_t size) {
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
