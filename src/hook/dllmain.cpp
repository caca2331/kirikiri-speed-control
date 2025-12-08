#include "XAudio2Hook.h"
#include "DirectSoundHook.h"
#include "HookUtils.h"
#include "../common/Logging.h"
#include <thread>
#include <Windows.h>
#include <cstring>

namespace {
    FARPROC (WINAPI *g_origGetProcAddress)(HMODULE, LPCSTR) = nullptr;

    FARPROC WINAPI GetProcAddressHook(HMODULE module, LPCSTR procName) {
        FARPROC fn = g_origGetProcAddress ? g_origGetProcAddress(module, procName) : nullptr;
        if (!procName || !fn) {
            return fn;
        }

        if (_stricmp(procName, "XAudio2Create") == 0) {
            auto &xa = krkrspeed::XAudio2Hook::instance();
            xa.setOriginalCreate(reinterpret_cast<void *>(fn));
            return reinterpret_cast<FARPROC>(&krkrspeed::XAudio2Hook::XAudio2CreateHook);
        }
        if (_stricmp(procName, "CoCreateInstance") == 0) {
            return reinterpret_cast<FARPROC>(&krkrspeed::XAudio2Hook::CoCreateInstanceHook);
        }
        if (_stricmp(procName, "DirectSoundCreate8") == 0) {
            auto &ds = krkrspeed::DirectSoundHook::instance();
            ds.setOriginalCreate8(reinterpret_cast<void *>(fn));
            return reinterpret_cast<FARPROC>(&krkrspeed::DirectSoundHook::DirectSoundCreate8Hook);
        }
        return fn;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        wchar_t skipFlag[8] = {};
        if (GetEnvironmentVariableW(L"KRKR_SKIP_HOOK_INIT", skipFlag, static_cast<DWORD>(std::size(skipFlag))) > 0) {
            KRKR_LOG_INFO("KRKR_SKIP_HOOK_INIT set; skipping hook setup in this process");
            return TRUE;
        }

        DisableThreadLibraryCalls(hModule);
        std::thread([] {
            KRKR_LOG_INFO("krkr_speed_hook.dll attached; starting hook initialization");

            if (krkrspeed::PatchImport("kernel32.dll", "GetProcAddress",
                                       reinterpret_cast<void *>(&GetProcAddressHook),
                                       reinterpret_cast<void **>(&g_origGetProcAddress))) {
                KRKR_LOG_INFO("Patched GetProcAddress import to catch dynamic audio API resolution");
            } else {
                KRKR_LOG_WARN("Failed to patch GetProcAddress; dynamic API resolution may bypass hooks");
            }

            krkrspeed::XAudio2Hook::instance().initialize();
            krkrspeed::DirectSoundHook::instance().initialize();
            KRKR_LOG_INFO("Hook initialization thread finished");
        }).detach();
    }
    return TRUE;
}
