#include "WaveOutHook.h"
#include "HookUtils.h"
#include "../common/Logging.h"
#include "XAudio2Hook.h"

#include <algorithm>
#include <vector>
#include <cstring>

namespace krkrspeed {

WaveOutHook &WaveOutHook::instance() {
    static WaveOutHook hook;
    return hook;
}

void WaveOutHook::initialize() {
    auto envFlagOn = [](const wchar_t *name) {
        wchar_t buf[8] = {};
        DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
        if (n == 0 || n >= std::size(buf)) return false;
        return wcscmp(buf, L"1") == 0;
    };
    if (envFlagOn(L"KRKR_SKIP_WAVEOUT")) {
        KRKR_LOG_INFO("KRKR_SKIP_WAVEOUT set; waveOut hooks disabled");
        return;
    }
    hookEntryPoints();
    KRKR_LOG_INFO("waveOut hook initialization done");
}

void WaveOutHook::hookEntryPoints() {
    PatchImport("winmm.dll", "waveOutOpen", reinterpret_cast<void *>(&WaveOutHook::waveOutOpenHook),
                reinterpret_cast<void **>(&m_origOpen));
    PatchImport("winmm.dll", "waveOutWrite", reinterpret_cast<void *>(&WaveOutHook::waveOutWriteHook),
                reinterpret_cast<void **>(&m_origWrite));
}

void WaveOutHook::setOriginalOpen(void *fn) {
    if (!fn || m_origOpen) return;
    m_origOpen = reinterpret_cast<PFN_waveOutOpen>(fn);
    KRKR_LOG_DEBUG("Captured waveOutOpen via GetProcAddress");
}

void WaveOutHook::setOriginalWrite(void *fn) {
    if (!fn || m_origWrite) return;
    m_origWrite = reinterpret_cast<PFN_waveOutWrite>(fn);
    KRKR_LOG_DEBUG("Captured waveOutWrite via GetProcAddress");
}

MMRESULT WINAPI WaveOutHook::waveOutOpenHook(LPHWAVEOUT phwo, UINT uDeviceID, LPCWAVEFORMATEX pwfx,
                                             DWORD_PTR dwCallback, DWORD_PTR dwInstance, DWORD fdwOpen) {
    auto &hook = WaveOutHook::instance();
    if (!hook.m_origOpen) return MMSYSERR_ERROR;

    MMRESULT res = hook.m_origOpen(phwo, uDeviceID, pwfx, dwCallback, dwInstance, fdwOpen);
    if (res != MMSYSERR_NOERROR || !phwo || !pwfx) return res;

    WaveState state;
    state.sampleRate = pwfx->nSamplesPerSec;
    state.channels = pwfx->nChannels;
    state.bitsPerSample = pwfx->wBitsPerSample;
    state.isPcm16 = (pwfx->wFormatTag == WAVE_FORMAT_PCM && pwfx->wBitsPerSample == 16);
    DspConfig cfg{};
    if (state.isPcm16) {
        state.dsp = std::make_unique<DspPipeline>(state.sampleRate, state.channels, cfg);
    }
    {
        std::lock_guard<std::mutex> lock(hook.m_mutex);
        hook.m_states[*phwo] = std::move(state);
    }
    KRKR_LOG_INFO("waveOutOpen hooked sr=" + std::to_string(state.sampleRate) + " ch=" + std::to_string(state.channels) +
                  " fmt=" + std::to_string(pwfx->wFormatTag) + " bits=" + std::to_string(pwfx->wBitsPerSample));
    return res;
}

MMRESULT WINAPI WaveOutHook::waveOutWriteHook(HWAVEOUT hwo, LPWAVEHDR pwh) {
    auto &hook = WaveOutHook::instance();
    if (!hook.m_origWrite || !pwh || !pwh->lpData || pwh->dwBufferLength == 0) {
        return MMSYSERR_ERROR;
    }
    XAudio2Hook::instance().pollSharedSettings();

    WaveState *state = nullptr;
    {
        std::lock_guard<std::mutex> lock(hook.m_mutex);
        auto it = hook.m_states.find(hwo);
        if (it != hook.m_states.end()) state = &it->second;
    }
    if (!state || !state->dsp || !state->isPcm16) {
        // Unknown format; just pass through.
        return hook.m_origWrite(hwo, pwh);
    }

    const float userSpeed = XAudio2Hook::instance().getUserSpeed();
    const bool gate = XAudio2Hook::instance().isLengthGateEnabled();
    const float gateSeconds = XAudio2Hook::instance().lengthGateSeconds();

    const std::size_t frames = (pwh->dwBufferLength / sizeof(std::int16_t)) / std::max<std::uint32_t>(1, state->channels);
    const float durationSec = static_cast<float>(frames) / std::max<float>(1.0f, static_cast<float>(state->sampleRate));
    if (gate && durationSec > gateSeconds) {
        return hook.m_origWrite(hwo, pwh);
    }

    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        KRKR_LOG_INFO("waveOutWriteHook engaged hwo=" + std::to_string(reinterpret_cast<std::uintptr_t>(hwo)) +
                      " sr=" + std::to_string(state->sampleRate) + " ch=" + std::to_string(state->channels));
    }

    static bool disableDsp = []{
        wchar_t buf[8] = {};
        DWORD n = GetEnvironmentVariableW(L"KRKR_DISABLE_DSP", buf, static_cast<DWORD>(std::size(buf)));
        return (n > 0 && n < std::size(buf) && wcscmp(buf, L"1") == 0);
    }();
    static bool passthrough = []{
        wchar_t buf[8] = {};
        DWORD n = GetEnvironmentVariableW(L"KRKR_WAVEOUT_PASSTHROUGH", buf, static_cast<DWORD>(std::size(buf)));
        return (n > 0 && n < std::size(buf) && wcscmp(buf, L"1") == 0);
    }();
    if (disableDsp || passthrough) {
        return hook.m_origWrite(hwo, pwh);
    }

    std::vector<std::uint8_t> processed = state->dsp->process(reinterpret_cast<const std::uint8_t *>(pwh->lpData),
                                                              pwh->dwBufferLength, userSpeed);
    if (processed.empty()) {
        return hook.m_origWrite(hwo, pwh);
    }
    // Overwrite in-place, clamp to original length.
    const std::size_t copyBytes = std::min<std::size_t>(processed.size(), pwh->dwBufferLength);
    std::memcpy(pwh->lpData, processed.data(), copyBytes);
    if (copyBytes < pwh->dwBufferLength) {
        std::memset(pwh->lpData + copyBytes, 0, pwh->dwBufferLength - copyBytes);
    }
    return hook.m_origWrite(hwo, pwh);
}

} // namespace krkrspeed
