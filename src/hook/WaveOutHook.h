#pragma once

#include <windows.h>
#include <mmsystem.h>
#include <map>
#include <mutex>
#include "../common/DspPipeline.h"
#include "../common/VoiceContext.h"

namespace krkrspeed {

class WaveOutHook {
public:
    static WaveOutHook &instance();
    void initialize();

    void setOriginalOpen(void *fn);
    void setOriginalWrite(void *fn);

    static MMRESULT WINAPI waveOutOpenHook(LPHWAVEOUT phwo, UINT uDeviceID, LPCWAVEFORMATEX pwfx,
                                           DWORD_PTR dwCallback, DWORD_PTR dwInstance, DWORD fdwOpen);
    static MMRESULT WINAPI waveOutWriteHook(HWAVEOUT hwo, LPWAVEHDR pwh);

private:
    WaveOutHook() = default;
    void hookEntryPoints();

    using PFN_waveOutOpen = MMRESULT(WINAPI *)(LPHWAVEOUT, UINT, LPCWAVEFORMATEX, DWORD_PTR, DWORD_PTR, DWORD);
    using PFN_waveOutWrite = MMRESULT(WINAPI *)(HWAVEOUT, LPWAVEHDR);

    PFN_waveOutOpen m_origOpen = nullptr;
    PFN_waveOutWrite m_origWrite = nullptr;

    struct WaveState {
        std::uint32_t sampleRate = 0;
        std::uint32_t channels = 0;
        std::uint16_t bitsPerSample = 16;
        bool isPcm16 = true;
        std::unique_ptr<DspPipeline> dsp;
    };
    std::map<HWAVEOUT, WaveState> m_states;
    std::mutex m_mutex;
};

} // namespace krkrspeed
