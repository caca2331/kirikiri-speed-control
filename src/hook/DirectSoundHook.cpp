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
    static auto envFloat = [](const wchar_t *name, float fallback) {
        wchar_t buf[32] = {};
        DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
        if (n == 0 || n >= std::size(buf)) return fallback;
        try {
            return std::stof(std::wstring(buf));
        } catch (...) {
            return fallback;
        }
    };
    auto envFlagOff = [&](const wchar_t *name) {
        wchar_t buf[8] = {};
        DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
        if (n == 0 || n >= std::size(buf)) return false;
        return wcscmp(buf, L"1") == 0;
    };
    // Default ON; allow opt-out with KRKR_SKIP_DS=1 for troubleshooting.
    if (envFlagOn(L"KRKR_SKIP_DS")) {
        KRKR_LOG_INFO("KRKR_SKIP_DS set; DirectSound hooks disabled");
        return;
    }
    m_bgmSecondsGate = envFloat(L"KRKR_DS_BGM_SECS", 60.0f);
    m_disableBgm = envFlagOn(L"KRKR_DS_DISABLE_BGM");
    m_forceApply = envFlagOn(L"KRKR_DS_FORCE");
    KRKR_LOG_INFO("DirectSound hook initialization started");
    hookEntryPoints();
    scanLoadedModules();
    bootstrapVtable();
#if !defined(_WIN64)
    installGlobalUnlockHook();
#else
    KRKR_LOG_INFO("Global Unlock hook skipped on x64 build");
#endif
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

    hook.patchDeviceVtable(*ppDS8);
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
    hook.patchDeviceVtable(ds8);
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
    patchDeviceVtable(ds8);
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
    KRKR_LOG_INFO("DS CreateSoundBuffer " + fmtKey + " buffer=" +
                  std::to_string(reinterpret_cast<std::uintptr_t>(*ppDSBuffer)));
    const bool isPrimary = (pcDSBufferDesc->dwFlags & DSBCAPS_PRIMARYBUFFER) != 0;
    const bool isPcm16 = fmt->wFormatTag == WAVE_FORMAT_PCM && fmt->wBitsPerSample == 16;
    const std::uint32_t blockAlign = fmt->nBlockAlign ? fmt->nBlockAlign : (fmt->nChannels * fmt->wBitsPerSample / 8);
    float approxSeconds = 0.0f;
    if (blockAlign > 0 && fmt->nSamplesPerSec > 0 && pcDSBufferDesc->dwBufferBytes > 0) {
        approxSeconds = static_cast<float>(pcDSBufferDesc->dwBufferBytes) /
                        static_cast<float>(blockAlign * fmt->nSamplesPerSec);
    }
    const bool likelyBgm = approxSeconds >= hook.m_bgmSecondsGate;
    {
        std::lock_guard<std::mutex> lock(hook.m_mutex);
        if (hook.m_loggedFormats.insert(fmtKey).second) {
            KRKR_LOG_INFO("DS CreateSoundBuffer " + fmtKey);
        }
    }

    if (isPrimary) {
        KRKR_LOG_INFO("Skip DirectSound Unlock patch on primary buffer");
        return hr;
    }
    if (!isPcm16) {
        KRKR_LOG_WARN("Skip Unlock patch: buffer is not PCM16 (" + fmtKey + ")");
        return hr;
    }

    hook.patchBufferVtable(*ppDSBuffer);

    // Track buffer format.
    std::lock_guard<std::mutex> lock(hook.m_mutex);
    BufferInfo info;
    info.sampleRate = fmt->nSamplesPerSec;
    info.channels = fmt->nChannels;
    info.bitsPerSample = fmt->wBitsPerSample;
    info.formatTag = fmt->wFormatTag;
    info.baseFrequency = fmt->nSamplesPerSec;
    info.bufferBytes = pcDSBufferDesc->dwBufferBytes;
    info.blockAlign = blockAlign;
    info.approxSeconds = approxSeconds;
    info.isLikelyBgm = likelyBgm;
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
    static bool disableDsp = []{
        wchar_t buf[8] = {};
        DWORD n = GetEnvironmentVariableW(L"KRKR_DISABLE_DSP", buf, static_cast<DWORD>(std::size(buf)));
        return (n > 0 && n < std::size(buf) && wcscmp(buf, L"1") == 0);
    }();
    static bool useWsola = []{
        wchar_t buf[8] = {};
        DWORD n = GetEnvironmentVariableW(L"KRKR_DS_WSOLA", buf, static_cast<DWORD>(std::size(buf)));
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
        KRKR_LOG_DEBUG("DS Unlock: combined buffer empty");
        return m_origUnlock(self, pAudioPtr1, dwAudioBytes1, pAudioPtr2, dwAudioBytes2);
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
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
            auto &info = it->second;
            info.unlockCount++;

            const std::size_t frames = (combined.size() / sizeof(std::int16_t)) /
                                       std::max<std::uint32_t>(1, info.channels);
            const float durationSec =
                static_cast<float>(frames) / static_cast<float>(std::max<std::uint32_t>(1, info.sampleRate));
            const float totalSec =
                static_cast<float>(info.processedFrames + frames) / static_cast<float>(std::max<std::uint32_t>(1, info.sampleRate));
            // skip only the very first tiny chunk (<0.5s) of a new buffer; once total >0.5s, process normally
            const bool tooShort = (durationSec < 0.5f) && (totalSec < 0.5f);
            const bool shouldLog = info.unlockCount <= 5 || (info.unlockCount % 50 == 0);
            // Loop-based BGM detection: only for large/looping buffers, skip small voice buffers.
            const float loopDetectThreshold = std::max<float>(2.0f, m_bgmSecondsGate * 0.25f);
            const bool musicCandidate = (info.channels > 1) || (info.approxSeconds >= 1.5f);
            // Fast-path: short stereo loops are very likely BGM; mark after a couple unlocks.
            if (!m_disableBgm && info.channels > 1 && info.approxSeconds <= 0.30f && info.unlockCount >= 2 &&
                !info.isLikelyBgm) {
                info.isLikelyBgm = true;
                if (shouldLog) {
                    KRKR_LOG_INFO("DS buffer marked BGM via stereo-short heuristic buf=" +
                                  std::to_string(reinterpret_cast<std::uintptr_t>(self)) +
                                  " dur=" + std::to_string(info.approxSeconds) +
                                  " unlocks=" + std::to_string(info.unlockCount));
                }
            }
            if (m_loopDetect && !m_disableBgm && !info.isLikelyBgm && info.bufferBytes > 0 &&
                info.approxSeconds >= loopDetectThreshold && musicCandidate) {
                info.loopBytesAccum += combined.size();
                const bool oneLoop = info.loopBytesAccum >= static_cast<std::uint64_t>(info.bufferBytes);
                const bool twoLoops = info.loopBytesAccum >= static_cast<std::uint64_t>(info.bufferBytes) * 2ULL;
                if ((oneLoop && info.unlockCount >= 4) || (twoLoops && totalSec >= 2.0f)) {
                    info.isLikelyBgm = true;
                    if (shouldLog) {
                        KRKR_LOG_INFO("DS buffer marked BGM via loop detect early buf=" +
                                      std::to_string(reinterpret_cast<std::uintptr_t>(self)) +
                                      " totalSec=" + std::to_string(totalSec) +
                                      " loopsBytes=" + std::to_string(info.loopBytesAccum) + "/" +
                                      std::to_string(info.bufferBytes));
                    }
                } else if (totalSec >= m_bgmSecondsGate && twoLoops && info.unlockCount >= 12) {
                    info.isLikelyBgm = true;
                    if (shouldLog) {
                        KRKR_LOG_INFO("DS buffer marked BGM via loop detection buf=" +
                                      std::to_string(reinterpret_cast<std::uintptr_t>(self)) +
                                      " totalSec=" + std::to_string(totalSec) +
                                      " loopsBytes=" + std::to_string(info.loopBytesAccum) + "/" +
                                      std::to_string(info.bufferBytes * 2ULL));
                    }
                }
            } else if (m_loopDetect && !m_disableBgm && !info.isLikelyBgm && musicCandidate) {
                // Streaming/rolling small buffers: early mark if they churn rapidly.
                if ((info.unlockCount >= 8 && totalSec >= 3.0f && info.approxSeconds <= 1.5f) ||
                    (totalSec >= 4.0f && info.unlockCount >= 12) ||
                    (totalSec >= m_bgmSecondsGate && info.unlockCount >= 20)) {
                    info.isLikelyBgm = true;
                    if (shouldLog) {
                        KRKR_LOG_INFO("DS buffer marked BGM via duration buf=" +
                                      std::to_string(reinterpret_cast<std::uintptr_t>(self)) +
                                      " totalSec=" + std::to_string(totalSec) +
                                      " unlocks=" + std::to_string(info.unlockCount));
                    }
                }
            }
            // Treat all stereo buffers as BGM candidates by default; only process if forced.
            const bool isBgm = ((info.channels > 1) || info.isLikelyBgm) && !m_disableBgm;
            const bool doDsp = !disableDsp && !tooShort && (!isBgm || m_forceApply) && (!gate || totalSec <= gateSeconds);
            if (shouldLog) {
                KRKR_LOG_DEBUG("DS Unlock: buf=" + std::to_string(reinterpret_cast<std::uintptr_t>(self)) +
                               " bytes=" + std::to_string(combined.size()) +
                               " ch=" + std::to_string(info.channels) +
                               " sr=" + std::to_string(info.sampleRate) +
                               " dur=" + std::to_string(durationSec) +
                               " total=" + std::to_string(totalSec) +
                               " bgm=" + (isBgm ? "1" : "0") +
                               " short=" + (tooShort ? "1" : "0") +
                               " apply=" + (doDsp ? "1" : "0") +
                               " speed=" + std::to_string(userSpeed));
            }
            auto padSilence = [](std::vector<std::uint8_t> &out, std::size_t targetBytes) {
                if (out.size() >= targetBytes) return;
                std::size_t old = out.size();
                out.resize(targetBytes);
                std::fill(out.begin() + old, out.end(), 0);
            };

            if (doDsp) {
                auto processTempo = [&]() {
                    auto out = info.dsp->process(combined.data(), combined.size(), userSpeed, DspMode::Tempo);
                    if (out.empty()) {
                        if (shouldLog) {
                            KRKR_LOG_DEBUG("DS DSP (WSOLA) produced 0 bytes; passthrough for buf=" +
                                           std::to_string(reinterpret_cast<std::uintptr_t>(self)));
                        }
                        return;
                    }
                    padSilence(out, combined.size());
                    if (out.size() >= combined.size()) {
                        std::copy_n(out.data(), combined.size(), combined.begin());
                    } else {
                        std::copy(out.begin(), out.end(), combined.begin());
                        std::fill(combined.begin() + out.size(), combined.end(), 0);
                    }
                    if (shouldLog) {
                        KRKR_LOG_DEBUG("DS WSOLA applied: in=" + std::to_string(combined.size()) +
                                       " out=" + std::to_string(out.size()) +
                                       " buf=" + std::to_string(reinterpret_cast<std::uintptr_t>(self)));
                    }
                };

                if (useWsola) {
                    processTempo();
                } else {
                    // Cap SetFrequency so target Hz never exceeds DSBFREQUENCY_MAX; use applied speed for pitch restore.
                    const DWORD base = info.baseFrequency ? info.baseFrequency : info.sampleRate;
                    const double scaled = static_cast<double>(base) * static_cast<double>(userSpeed);
                    double clampedD = scaled;
                    if (clampedD < static_cast<double>(DSBFREQUENCY_MIN)) clampedD = static_cast<double>(DSBFREQUENCY_MIN);
                    if (clampedD > static_cast<double>(DSBFREQUENCY_MAX)) clampedD = static_cast<double>(DSBFREQUENCY_MAX);
                    const DWORD clamped = static_cast<DWORD>(clampedD);
                    self->SetFrequency(clamped);
                    const float appliedSpeed =
                        base > 0 ? static_cast<float>(clampedD) / static_cast<float>(base) : userSpeed;
                    float denom = appliedSpeed;
                    if (denom < 0.01f) denom = 0.01f;
                    const float pitchDown = 1.0f / denom;
                    auto out = info.dsp->process(combined.data(), combined.size(), pitchDown, DspMode::Pitch);
                    if (out.empty()) {
                        if (shouldLog) {
                            KRKR_LOG_DEBUG("DS pitch-compensate produced 0 bytes; fallback passthrough buf=" +
                                           std::to_string(reinterpret_cast<std::uintptr_t>(self)));
                        }
                    } else {
                    padSilence(out, combined.size());
                    if (out.size() >= combined.size()) {
                        std::copy_n(out.data(), combined.size(), combined.begin());
                    } else {
                        std::copy(out.begin(), out.end(), combined.begin());
                        std::fill(combined.begin() + out.size(), combined.end(), 0);
                        }
                        if (shouldLog) {
                            KRKR_LOG_DEBUG("DS SetFrequency applied: base=" + std::to_string(base) +
                                           " target=" + std::to_string(clamped) +
                                           " appliedSpeed=" + std::to_string(appliedSpeed) +
                                           " pitchCompOut=" + std::to_string(out.size()));
                        }
                    }
                }
            }
            info.processedFrames += frames;
            break; // processed successfully
        } else {
            // Unknown buffer: try to discover format and start tracking, then loop to process.
            WAVEFORMATEX wfx{};
            DWORD cb = 0;
            if (SUCCEEDED(self->GetFormat(nullptr, 0, &cb)) && cb >= sizeof(WAVEFORMATEX)) {
                std::vector<std::uint8_t> fmtBuf(cb);
                if (SUCCEEDED(self->GetFormat(reinterpret_cast<LPWAVEFORMATEX>(fmtBuf.data()), cb, nullptr))) {
                    const auto *fx = reinterpret_cast<const WAVEFORMATEX *>(fmtBuf.data());
                    BufferInfo info;
                    info.sampleRate = fx->nSamplesPerSec;
                    info.channels = fx->nChannels;
                    info.bitsPerSample = fx->wBitsPerSample;
                    info.formatTag = fx->wFormatTag;
                    info.baseFrequency = fx->nSamplesPerSec;
                    info.blockAlign = fx->nBlockAlign ? fx->nBlockAlign : (fx->nChannels * fx->wBitsPerSample / 8);
                    // Estimate duration from buffer caps if available.
                    DSBCAPS caps{};
                    caps.dwSize = sizeof(caps);
                    DWORD bytes = 0;
                    if (SUCCEEDED(self->GetCaps(&caps))) {
                        bytes = caps.dwBufferBytes;
                    }
                    info.bufferBytes = bytes;
                    if (info.blockAlign > 0 && fx->nSamplesPerSec > 0 && bytes > 0) {
                        const float approxSeconds = static_cast<float>(bytes) /
                                                    static_cast<float>(info.blockAlign * fx->nSamplesPerSec);
                        info.isLikelyBgm = approxSeconds >= m_bgmSecondsGate;
                    }
                    // Start loop accumulation at current chunk so first detection works even if unlock happened before hook.
                    info.loopBytesAccum = combined.size();
                    info.isPcm16 = (fx->wFormatTag == WAVE_FORMAT_PCM && fx->wBitsPerSample == 16);
                    info.loggedFormat = false;
                    DspConfig cfg{};
                    info.dsp = std::make_unique<DspPipeline>(info.sampleRate, info.channels, cfg);
                    m_buffers[reinterpret_cast<std::uintptr_t>(self)] = std::move(info);
                    KRKR_LOG_INFO("DS Unlock: tracked buffer=" +
                                  std::to_string(reinterpret_cast<std::uintptr_t>(self)) +
                                  " fmt=" + std::to_string(fx->wFormatTag) +
                                  " bits=" + std::to_string(fx->wBitsPerSample) +
                                  " ch=" + std::to_string(fx->nChannels) +
                                  " sr=" + std::to_string(fx->nSamplesPerSec));
                    // Patch this buffer's vtable via shadow to ensure future calls are ours.
                    patchBufferVtable(self);
                } else {
                    KRKR_LOG_WARN("DS Unlock: GetFormat failed for untracked buffer; passthrough");
                    return m_origUnlock(self, pAudioPtr1, dwAudioBytes1, pAudioPtr2, dwAudioBytes2);
                }
            } else {
                KRKR_LOG_WARN("DS Unlock: GetFormat size query failed for untracked buffer; passthrough");
                return m_origUnlock(self, pAudioPtr1, dwAudioBytes1, pAudioPtr2, dwAudioBytes2);
            }
            auto it2 = m_buffers.find(reinterpret_cast<std::uintptr_t>(self));
            if (it2 == m_buffers.end() || !it2->second.dsp) {
                KRKR_LOG_WARN("DS Unlock: tracking failed; passthrough");
                return m_origUnlock(self, pAudioPtr1, dwAudioBytes1, pAudioPtr2, dwAudioBytes2);
            }
            if (!it2->second.isPcm16) {
                KRKR_LOG_WARN("DirectSound buffer format not PCM16 (after late track); skipping DSP. fmt=" +
                              std::to_string(it2->second.formatTag) + " bits=" +
                              std::to_string(it2->second.bitsPerSample) + " ch=" +
                              std::to_string(it2->second.channels) + " sr=" +
                              std::to_string(it2->second.sampleRate));
                return m_origUnlock(self, pAudioPtr1, dwAudioBytes1, pAudioPtr2, dwAudioBytes2);
            }
            // Loop again to process with freshly tracked info.
            continue;
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

void DirectSoundHook::patchDeviceVtable(IDirectSound8 *ds8) {
    if (!ds8) return;
    if (m_disableVtablePatch) {
        KRKR_LOG_INFO("KRKR_DS_DISABLE_VTABLE set; skipping device vtable patch");
        return;
    }
    std::lock_guard<std::mutex> lock(m_vtableMutex);
    if (m_deviceVtables.find(ds8) != m_deviceVtables.end()) {
        return;
    }
    void **origVtbl = *reinterpret_cast<void ***>(ds8);
    if (!origVtbl) return;

    if (!m_origCreateBuffer) {
        m_origCreateBuffer = reinterpret_cast<PFN_CreateSoundBuffer>(origVtbl[3]);
    }

    constexpr size_t kCount = 32;
    std::vector<void *> shadow(kCount);
    for (size_t i = 0; i < kCount; ++i) shadow[i] = origVtbl[i];
    shadow[3] = reinterpret_cast<void *>(&DirectSoundHook::CreateSoundBufferHook);

    *reinterpret_cast<void ***>(ds8) = shadow.data();
    m_deviceVtables[ds8] = std::move(shadow);
    KRKR_LOG_INFO("Applied shadow vtable for IDirectSound8 instance (CreateSoundBuffer)");
}

void DirectSoundHook::patchBufferVtable(IDirectSoundBuffer *buf) {
    if (!buf) return;
    if (m_disableVtablePatch) {
        KRKR_LOG_INFO("KRKR_DS_DISABLE_VTABLE set; skipping buffer vtable patch");
        return;
    }
    std::lock_guard<std::mutex> lock(m_vtableMutex);
    if (m_bufferVtables.find(buf) != m_bufferVtables.end()) {
        return;
    }
    void **origVtbl = *reinterpret_cast<void ***>(buf);
    if (!origVtbl) return;

    if (!m_origUnlock) {
        m_origUnlock = reinterpret_cast<PFN_Unlock>(origVtbl[19]);
    }

    constexpr size_t kCount = 32;
    std::vector<void *> shadow(kCount);
    for (size_t i = 0; i < kCount; ++i) shadow[i] = origVtbl[i];
    shadow[19] = reinterpret_cast<void *>(&DirectSoundHook::UnlockHook);

    *reinterpret_cast<void ***>(buf) = shadow.data();
    m_bufferVtables[buf] = std::move(shadow);
    KRKR_LOG_INFO("Applied shadow vtable for IDirectSoundBuffer instance (Unlock)");
}

void DirectSoundHook::installGlobalUnlockHook() {
#if defined(_WIN64)
    return;
#else
    // Build a temporary DS buffer to discover the real Unlock implementation.
    if (!m_origCreate8 && !m_origCreate) return;
    IDirectSound8 *ds8 = nullptr;
    if (m_origCreate8) {
        if (FAILED(m_origCreate8(nullptr, &ds8, nullptr)) || !ds8) return;
    } else {
        IDirectSound *ds = nullptr;
        if (FAILED(m_origCreate(nullptr, &ds, nullptr)) || !ds) return;
        ds->QueryInterface(IID_IDirectSound8, reinterpret_cast<void **>(&ds8));
        ds->Release();
        if (!ds8) return;
    }
    ds8->SetCooperativeLevel(GetDesktopWindow(), DSSCL_PRIORITY);
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = 44100;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
    wfx.nAvgBytesPerSec = wfx.nBlockAlign * wfx.nSamplesPerSec;
    DSBUFFERDESC desc{};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_GETCURRENTPOSITION2;
    desc.dwBufferBytes = wfx.nAvgBytesPerSec / 2;
    desc.lpwfxFormat = &wfx;
    IDirectSoundBuffer *tmp = nullptr;
    if (FAILED(ds8->CreateSoundBuffer(&desc, &tmp, nullptr)) || !tmp) {
        ds8->Release();
        return;
    }
    void **vtbl = *reinterpret_cast<void ***>(tmp);
    void *target = vtbl ? vtbl[19] : nullptr;
    tmp->Release();
    ds8->Release();
    if (!target) return;

    // Prepare trampoline.
    static BYTE saved[5]{};
    static BYTE *trampoline = nullptr;
    static PFN_Unlock orig = nullptr;
    if (orig) return; // already installed
    orig = m_origUnlock ? m_origUnlock : reinterpret_cast<PFN_Unlock>(target);

    DWORD oldProtect = 0;
    VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
    std::memcpy(saved, target, 5);

    trampoline = reinterpret_cast<BYTE *>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        VirtualProtect(target, 5, oldProtect, &oldProtect);
        return;
    }
    // trampoline: original bytes + jmp back
    std::memcpy(trampoline, saved, 5);
    intptr_t backRel = (reinterpret_cast<BYTE *>(target) + 5) - (trampoline + 5) - 5;
    trampoline[5] = 0xE9;
    *reinterpret_cast<int32_t *>(trampoline + 6) = static_cast<int32_t>(backRel);

    // patch target to jump to our hook
    intptr_t rel = reinterpret_cast<BYTE *>(&DirectSoundHook::UnlockHook) - (reinterpret_cast<BYTE *>(target) + 5);
    BYTE patch[5] = {0xE9};
    *reinterpret_cast<int32_t *>(patch + 1) = static_cast<int32_t>(rel);
    std::memcpy(target, patch, 5);
    VirtualProtect(target, 5, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, 5);

    // update m_origUnlock to trampoline so UnlockHook can call through.
    m_origUnlock = reinterpret_cast<PFN_Unlock>(trampoline);
    KRKR_LOG_INFO("Installed global Unlock detour via inline jump");
#endif
}

} // namespace krkrspeed
