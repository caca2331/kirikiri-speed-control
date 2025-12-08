#include "XAudio2Hook.h"
#include "DirectSoundHook.h"
#include "../common/Logging.h"
#include <thread>
#include <Windows.h>

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        std::thread([] {
            KRKR_LOG_INFO("krkr_speed_hook.dll attached; starting hook initialization");
            krkrspeed::XAudio2Hook::instance().initialize();
            krkrspeed::DirectSoundHook::instance().initialize();
            KRKR_LOG_INFO("Hook initialization thread finished");
        }).detach();
    }
    return TRUE;
}
