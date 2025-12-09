#include "DirectSoundHook.h"
#include "HookUtils.h"
#include "XAudio2Hook.h"
#include "../common/Logging.h"

#include <initguid.h>
#include <algorithm>
#include <vector>
#include <cstring>
#include <cstdint>
#include <memory>
#include <Psapi.h>

namespace krkrspeed {

DirectSoundHook &DirectSoundHook::instance() {
    static DirectSoundHook hook;
    return hook;
}

void DirectSoundHook::initialize() {
    static auto envFlagOn = [](const wchar_t *name) {
        wchar_t buf[8] = {};
        DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
        if (n == 0 || n >= std::size(buf)) return false;
        return wcscmp(buf, L"1") == 0;
    };
    // Opt-in only: require KRKR_ENABLE_DS=1 to activate DS hooks.
    if (!envFlagOn(L"KRKR_ENABLE_DS")) {
        KRKR_LOG_INFO("KRKR_ENABLE_DS not set; DirectSound hooks disabled");
        return;
    }
    m_disableVtablePatch = envFlagOn(L"KRKR_DS_DISABLE_VTABLE");
    m_logOnly = envFlagOn(L"KRKR_DS_LOG_ONLY");
    KRKR_LOG_INFO("DirectSound hook initialization started");
    hookEntryPoints();
    scanLoadedModules();
    bootstrapVtable();
}

void DirectSoundHook::setOriginalCreate8(void *fn) {
    if (!fn || m_origCreate8) {
        return;
    }
    m_origCreate8 = reinterpret_cast<PFN_DirectSoundCreate8>(fn);
    KRKR_LOG_DEBUG("Captured DirectSoundCreate8 via GetProcAddress; enabling DirectSound interception");
}

void DirectSoundHook::setOriginalCreate(void *fn) {
    if (!fn || m_origCreate) {
        return;
    }
    m_origCreate = reinterpret_cast<PFN_DirectSoundCreate>(fn);
    KRKR_LOG_DEBUG("Captured DirectSoundCreate via GetProcAddress; enabling DirectSound interception");
}

void DirectSoundHook::hookEntryPoints() {
    if (PatchImport("dsound.dll", "DirectSoundCreate8", reinterpret_cast<void *>(&DirectSoundHook::DirectSoundCreate8Hook),
                    reinterpret_cast<void **>(&m_origCreate8))) {
        KRKR_LOG_INFO("Patched DirectSoundCreate8 import");
    } else {
        KRKR_LOG_WARN("Failed to patch DirectSoundCreate8 import; will fall back to GetProcAddress interception");
    }
    PatchImport("dsound.dll", "DirectSoundCreate", reinterpret_cast<void *>(&DirectSoundHook::DirectSoundCreateHook),
                reinterpret_cast<void **>(&m_origCreate));
}

HRESULT WINAPI DirectSoundHook::DirectSoundCreate8Hook(LPCGUID pcGuidDevice, LPDIRECTSOUND8 *ppDS8,
                                                       LPUNKNOWN pUnkOuter) {
    auto &hook = DirectSoundHook::instance();
    if (!hook.m_origCreate8) {
        return DSERR_GENERIC;
    }
    HRESULT hr = hook.m_origCreate8(pcGuidDevice, ppDS8, pUnkOuter);
    if (FAILED(hr) || !ppDS8 || !*ppDS8) {
        return hr;
    }

    if (hook.m_disableVtablePatch) {
        KRKR_LOG_INFO("KRKR_DS_DISABLE_VTABLE set; skipping CreateSoundBuffer patch");
        return hr;
    }

    void **vtbl = *reinterpret_cast<void ***>(*ppDS8);
    if (!hook.m_origCreateBuffer) {
        PatchVtableEntry(vtbl, 3, &DirectSoundHook::CreateSoundBufferHook, hook.m_origCreateBuffer);
        KRKR_LOG_INFO("Patched IDirectSound8 vtable (CreateSoundBuffer)");
    }
    return hr;
}

HRESULT WINAPI DirectSoundHook::DirectSoundCreateHook(LPCGUID pcGuidDevice, LPDIRECTSOUND *ppDS, LPUNKNOWN pUnkOuter) {
    auto &hook = DirectSoundHook::instance();
    if (!hook.m_origCreate) {
        return DSERR_GENERIC;
    }
    IDirectSound *ds = nullptr;
    HRESULT hr = hook.m_origCreate(pcGuidDevice, &ds, pUnkOuter);
    if (FAILED(hr) || !ds) {
        return hr;
    }
    // Query for IDirectSound8 to reuse same flow.
    IDirectSound8 *ds8 = nullptr;
    hr = ds->QueryInterface(IID_IDirectSound8, reinterpret_cast<void **>(&ds8));
    ds->Release();
    if (FAILED(hr) || !ds8) {
        return hr;
    }
    if (hook.m_disableVtablePatch) {
        KRKR_LOG_INFO("KRKR_DS_DISABLE_VTABLE set; skipping vtable patch via DirectSoundCreate");
        *ppDS = ds8;
        return hr;
    }
    void **vtbl = *reinterpret_cast<void ***>(ds8);
    if (!hook.m_origCreateBuffer) {
        PatchVtableEntry(vtbl, 3, &DirectSoundHook::CreateSoundBufferHook, hook.m_origCreateBuffer);
        KRKR_LOG_INFO("Patched IDirectSound vtable (CreateSoundBuffer) via DirectSoundCreate");
    }
    *ppDS = ds8;
    return hr;
}

void DirectSoundHook::scanLoadedModules() {
    HMODULE modules[256];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) return;
    const size_t count = std::min<std::size_t>(needed / sizeof(HMODULE), std::size(modules));
    for (size_t i = 0; i < count; ++i) {
        char name[MAX_PATH] = {};
        if (GetModuleBaseNameA(GetCurrentProcess(), modules[i], name, static_cast<DWORD>(std::size(name))) == 0) continue;
        std::string lower(name);
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find("dsound") != std::string::npos) {
            if (!m_origCreate8) {
                if (auto fn = GetProcAddress(modules[i], "DirectSoundCreate8")) {
                    setOriginalCreate8(reinterpret_cast<void *>(fn));
                    KRKR_LOG_INFO(std::string("scanLoadedModules captured DirectSoundCreate8 from ") + name);
                }
            }
            if (!m_origCreate) {
                if (auto fn = GetProcAddress(modules[i], "DirectSoundCreate")) {
                    setOriginalCreate(reinterpret_cast<void *>(fn));
                    KRKR_LOG_INFO(std::string("scanLoadedModules captured DirectSoundCreate from ") + name);
                }
            }
        }
    }
}

void DirectSoundHook::bootstrapVtable() {
    // Patch shared vtable using a temporary DirectSound8 instance.
    if (!m_origCreate8 && !m_origCreate) return;
    IDirectSound8 *ds8 = nullptr;
    if (m_origCreate8) {
        if (FAILED(m_origCreate8(nullptr, &ds8, nullptr)) || !ds8) {
            return;
        }
    } else if (m_origCreate) {
        IDirectSound *ds = nullptr;
        if (FAILED(m_origCreate(nullptr, &ds, nullptr)) || !ds) {
            return;
        }
        ds->QueryInterface(IID_IDirectSound8, reinterpret_cast<void **>(&ds8));
        ds->Release();
        if (!ds8) return;
    }
    ds8->SetCooperativeLevel(GetDesktopWindow(), DSSCL_PRIORITY);
    if (!m_disableVtablePatch) {
        void **vtbl = *reinterpret_cast<void ***>(ds8);
        if (!m_origCreateBuffer) {
            PatchVtableEntry(vtbl, 3, &DirectSoundHook::CreateSoundBufferHook, m_origCreateBuffer);
            KRKR_LOG_INFO("Bootstrapped IDirectSound8 vtable (CreateSoundBuffer)");
        }
    } else {
        KRKR_LOG_INFO("KRKR_DS_DISABLE_VTABLE set; skipping bootstrap vtable patch");
    }
    ds8->Release();
}

HRESULT WINAPI DirectSoundHook::CreateSoundBufferHook(IDirectSound8 *self, LPDIRECTSOUNDBUFFER *ppDSBuffer,
                                                      LPCDSBUFFERDESC pcDSBufferDesc) {
    auto &hook = DirectSoundHook::instance();
    if (!hook.m_origCreateBuffer) {
        return DSERR_GENERIC;
    }
    HRESULT hr = hook.m_origCreateBuffer(self, ppDSBuffer, pcDSBufferDesc);
    if (FAILED(hr) || !ppDSBuffer || !*ppDSBuffer || !pcDSBufferDesc || !pcDSBufferDesc->lpwfxFormat) {
        return hr;
    }

    const auto *fmt = pcDSBufferDesc->lpwfxFormat;
    std::string fmtKey = "fmt=" + std::to_string(fmt->wFormatTag) + " bits=" + std::to_string(fmt->wBitsPerSample) +
                         " ch=" + std::to_string(fmt->nChannels) + " sr=" + std::to_string(fmt->nSamplesPerSec) +
                         " bytes=" + std::to_string(pcDSBufferDesc->dwBufferBytes) +
                         " flags=0x" + std::to_string(pcDSBufferDesc->dwFlags);
    const bool isPrimary = (pcDSBufferDesc->dwFlags & DSBCAPS_PRIMARYBUFFER) != 0;
    const bool isPcm16 = fmt->wFormatTag == WAVE_FORMAT_PCM && fmt->wBitsPerSample == 16;
    {
        std::lock_guard<std::mutex> lock(hook.m_mutex);
        if (hook.m_loggedFormats.insert(fmtKey).second) {
            KRKR_LOG_INFO("DS CreateSoundBuffer " + fmtKey + (hook.m_logOnly ? " [log-only]" : ""));
        }
    }

    if (hook.m_logOnly) {
        return hr;
    }

    if (isPrimary) {
        KRKR_LOG_INFO("Skip DirectSound Unlock patch on primary buffer");
        return hr;
    }
    if (!isPcm16) {
        KRKR_LOG_WARN("Skip Unlock patch: buffer is not PCM16 (" + fmtKey + ")");
        return hr;
    }

    void **vtbl = *reinterpret_cast<void ***>(*ppDSBuffer);
    if (!hook.m_origUnlock) {
        auto candidate = vtbl[19];
        HMODULE dsMod = GetModuleHandleA("dsound.dll");
        MODULEINFO mi{};
        if (dsMod && GetModuleInformation(GetCurrentProcess(), dsMod, &mi, sizeof(mi))) {
            const auto base = reinterpret_cast<std::uintptr_t>(mi.lpBaseOfDll);
            const auto end = base + mi.SizeOfImage;
            const auto addr = reinterpret_cast<std::uintptr_t>(candidate);
            if (addr >= base && addr < end) {
                PatchVtableEntry(vtbl, 19, &DirectSoundHook::UnlockHook, hook.m_origUnlock);
                KRKR_LOG_INFO("Patched IDirectSoundBuffer vtable (Unlock)");
            } else {
                KRKR_LOG_WARN("Skip Unlock patch: vtable entry not in dsound.dll");
            }
        } else {
            KRKR_LOG_WARN("Skip Unlock patch: cannot query dsound.dll module info");
        }
    }

    // Track buffer format.
    std::lock_guard<std::mutex> lock(hook.m_mutex);
    BufferInfo info;
    info.sampleRate = fmt->nSamplesPerSec;
    info.channels = fmt->nChannels;
    info.bitsPerSample = fmt->wBitsPerSample;
    info.formatTag = fmt->wFormatTag;
    info.isPcm16 = isPcm16;
    DspConfig cfg{};
    info.dsp = std::make_unique<DspPipeline>(info.sampleRate, info.channels, cfg);
    hook.m_buffers[reinterpret_cast<std::uintptr_t>(*ppDSBuffer)] = std::move(info);

    return hr;
}

HRESULT WINAPI DirectSoundHook::UnlockHook(IDirectSoundBuffer *self, LPVOID pAudioPtr1, DWORD dwAudioBytes1,
                                           LPVOID pAudioPtr2, DWORD dwAudioBytes2) {
    auto &hook = DirectSoundHook::instance();
    if (!hook.m_origUnlock) {
        return DSERR_GENERIC;
    }
    if (hook.m_disableAfterFault.load()) {
        return hook.m_origUnlock(self, pAudioPtr1, dwAudioBytes1, pAudioPtr2, dwAudioBytes2);
    }

    // Best-effort safety: if we hit any exception, disable further processing.
    try {
        return hook.handleUnlock(self, pAudioPtr1, dwAudioBytes1, pAudioPtr2, dwAudioBytes2);
    } catch (...) {
        if (!hook.m_disableAfterFault.exchange(true)) {
            KRKR_LOG_ERROR("DirectSound UnlockHook threw; disabling DS processing for safety");
        }
        return hook.m_origUnlock(self, pAudioPtr1, dwAudioBytes1, pAudioPtr2, dwAudioBytes2);
    }
}

HRESULT WINAPI DirectSoundHook::UnlockHook8(IDirectSoundBuffer8 *self, LPVOID pAudioPtr1, DWORD dwAudioBytes1,
                                            LPVOID pAudioPtr2, DWORD dwAudioBytes2) {
    return UnlockHook(reinterpret_cast<IDirectSoundBuffer *>(self), pAudioPtr1, dwAudioBytes1, pAudioPtr2, dwAudioBytes2);
}

HRESULT DirectSoundHook::handleUnlock(IDirectSoundBuffer *self, LPVOID pAudioPtr1, DWORD dwAudioBytes1,
                                      LPVOID pAudioPtr2, DWORD dwAudioBytes2) {
    static bool passthrough = []{
        wchar_t buf[8] = {};
        DWORD n = GetEnvironmentVariableW(L"KRKR_DS_PASSTHROUGH", buf, static_cast<DWORD>(std::size(buf)));
        return (n > 0 && n < std::size(buf) && wcscmp(buf, L"1") == 0);
    }();
    if (passthrough) {
        return m_origUnlock(self, pAudioPtr1, dwAudioBytes1, pAudioPtr2, dwAudioBytes2);
    }
    static bool disableDsp = []{
        wchar_t buf[8] = {};
        DWORD n = GetEnvironmentVariableW(L"KRKR_DISABLE_DSP", buf, static_cast<DWORD>(std::size(buf)));
        return (n > 0 && n < std::size(buf) && wcscmp(buf, L"1") == 0);
    }();
    if (!m_loggedUnlockOnce.exchange(true)) {
        KRKR_LOG_INFO("DirectSound UnlockHook engaged on buffer=" +
                      std::to_string(reinterpret_cast<std::uintptr_t>(self)));
    }
    XAudio2Hook::instance().pollSharedSettings();

    // Validate pointers; fall back to passthrough if unsafe.
    auto ptrInvalid = [](LPVOID ptr, DWORD bytes) {
        if (!ptr || bytes == 0) return false;
        return IsBadReadPtr(ptr, bytes) || IsBadWritePtr(ptr, bytes);
    };
    if (ptrInvalid(pAudioPtr1, dwAudioBytes1) || ptrInvalid(pAudioPtr2, dwAudioBytes2)) {
        KRKR_LOG_WARN("DirectSound UnlockHook detected invalid buffer pointers; falling back to passthrough");
        return m_origUnlock(self, pAudioPtr1, dwAudioBytes1, pAudioPtr2, dwAudioBytes2);
    }

    std::vector<std::uint8_t> combined;
    combined.reserve(dwAudioBytes1 + dwAudioBytes2);
    if (pAudioPtr1 && dwAudioBytes1) {
        auto *ptr = reinterpret_cast<std::uint8_t *>(pAudioPtr1);
        combined.insert(combined.end(), ptr, ptr + dwAudioBytes1);
    }
    if (pAudioPtr2 && dwAudioBytes2) {
        auto *ptr = reinterpret_cast<std::uint8_t *>(pAudioPtr2);
        combined.insert(combined.end(), ptr, ptr + dwAudioBytes2);
    }

    if (combined.empty()) {
        return m_origUnlock(self, pAudioPtr1, dwAudioBytes1, pAudioPtr2, dwAudioBytes2);
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_buffers.find(reinterpret_cast<std::uintptr_t>(self));
        if (it != m_buffers.end() && it->second.dsp) {
            if (!it->second.isPcm16) {
                if (!it->second.loggedFormat) {
                    KRKR_LOG_WARN("DirectSound buffer format not PCM16; skipping DSP. fmt=" +
                                  std::to_string(it->second.formatTag) + " bits=" +
                                  std::to_string(it->second.bitsPerSample) + " ch=" +
                                  std::to_string(it->second.channels) + " sr=" +
                                  std::to_string(it->second.sampleRate));
                    it->second.loggedFormat = true;
                }
                return m_origUnlock(self, pAudioPtr1, dwAudioBytes1, pAudioPtr2, dwAudioBytes2);
            }
            const float userSpeed = XAudio2Hook::instance().getUserSpeed();
            const bool gate = XAudio2Hook::instance().isLengthGateEnabled();
            const float gateSeconds = XAudio2Hook::instance().lengthGateSeconds();

            const std::size_t frames = (combined.size() / sizeof(std::int16_t)) /
                                       std::max<std::uint32_t>(1, it->second.channels);
            const float durationSec =
                static_cast<float>(frames) / static_cast<float>(std::max<std::uint32_t>(1, it->second.sampleRate));
            if (!disableDsp && (!gate || durationSec <= gateSeconds)) {
                auto out = it->second.dsp->process(combined.data(), combined.size(), userSpeed);
                if (!out.empty()) {
                    if (out.size() >= combined.size()) {
                        std::copy_n(out.data(), combined.size(), combined.begin());
                    } else {
                        std::copy(out.begin(), out.end(), combined.begin());
                        std::fill(combined.begin() + out.size(), combined.end(), 0);
                    }
                }
            }
        } else {
            // Unknown buffer; safety pass-through.
            return m_origUnlock(self, pAudioPtr1, dwAudioBytes1, pAudioPtr2, dwAudioBytes2);
        }
    }

    // Write combined buffer back into the two regions.
    std::size_t cursor = 0;
    if (pAudioPtr1 && dwAudioBytes1) {
        std::memcpy(pAudioPtr1, combined.data(), dwAudioBytes1);
        cursor += dwAudioBytes1;
    }
    if (pAudioPtr2 && dwAudioBytes2) {
        std::memcpy(pAudioPtr2, combined.data() + cursor, dwAudioBytes2);
    }

    return m_origUnlock(self, pAudioPtr1, dwAudioBytes1, pAudioPtr2, dwAudioBytes2);
}

} // namespace krkrspeed
