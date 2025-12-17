#include "XAudio2Hook.h"
#include "DirectSoundHook.h"
#include "FMODHook.h"
#include "WwiseHook.h"
#include "HookUtils.h"
#include "../common/Logging.h"
#include "../common/SharedSettings.h"
#include <thread>
#include <Windows.h>
#include <cstring>
#include <winternl.h>
#include <Psapi.h>

// Minimal LDR notification structs to avoid SDK version issues.
typedef struct _KRKR_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} KRKR_UNICODE_STRING, *PKRKR_UNICODE_STRING;

typedef struct _KRKR_LDR_DLL_LOADED_NOTIFICATION_DATA {
    ULONG Flags;
    const KRKR_UNICODE_STRING *FullDllName;
    const KRKR_UNICODE_STRING *BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} KRKR_LDR_DLL_LOADED_NOTIFICATION_DATA, *PKRKR_LDR_DLL_LOADED_NOTIFICATION_DATA;

typedef struct _KRKR_LDR_DLL_NOTIFICATION_DATA {
    ULONG NotificationReason;
    union {
        KRKR_LDR_DLL_LOADED_NOTIFICATION_DATA Loaded;
        KRKR_LDR_DLL_LOADED_NOTIFICATION_DATA Unloaded;
    };
} KRKR_LDR_DLL_NOTIFICATION_DATA, *PKRKR_LDR_DLL_NOTIFICATION_DATA;

namespace {
    FARPROC (WINAPI *g_origGetProcAddress)(HMODULE, LPCSTR) = nullptr;
    HMODULE (WINAPI *g_origLoadLibraryA)(LPCSTR) = nullptr;
    HMODULE (WINAPI *g_origLoadLibraryW)(LPCWSTR) = nullptr;
    HMODULE (WINAPI *g_origLoadLibraryExA)(LPCSTR, HANDLE, DWORD) = nullptr;
    HMODULE (WINAPI *g_origLoadLibraryExW)(LPCWSTR, HANDLE, DWORD) = nullptr;
    PVOID g_dllNotifyCookie = nullptr;
    PVOID g_vectored = nullptr;

    LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS *info) {
        static std::atomic<int> logged{0};
        if (!info || !info->ExceptionRecord) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        const DWORD code = info->ExceptionRecord->ExceptionCode;
        if (logged.fetch_add(1) < 5) {
            void *addr = info->ExceptionRecord->ExceptionAddress;
            HMODULE mod = nullptr;
            char modName[MAX_PATH] = {};
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(addr), &mod) && mod) {
                GetModuleBaseNameA(GetCurrentProcess(), mod, modName, static_cast<DWORD>(std::size(modName)));
            }
            KRKR_LOG_ERROR("Vectored exception code=0x" + std::to_string(code) +
                           " addr=" + std::to_string(reinterpret_cast<std::uintptr_t>(addr)) +
                           " mod=" + modName);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void OnLibraryLoaded(HMODULE module, const char *nameAnsi) {
        if (!module || !nameAnsi) return;
        auto &xa = krkrspeed::XAudio2Hook::instance();
        auto &ds = krkrspeed::DirectSoundHook::instance();
        if (!xa.hasCreateHook()) {
            FARPROC fn = GetProcAddress(module, "XAudio2Create");
            if (fn) {
                xa.setOriginalCreate(reinterpret_cast<void *>(fn));
                KRKR_LOG_INFO(std::string("Captured XAudio2Create from newly loaded module: ") + nameAnsi);
            }
        }
        if (!ds.hasCreateHook()) {
            FARPROC fn = GetProcAddress(module, "DirectSoundCreate8");
            if (fn) {
                ds.setOriginalCreate8(reinterpret_cast<void *>(fn));
                KRKR_LOG_INFO(std::string("Captured DirectSoundCreate8 from newly loaded module: ") + nameAnsi);
            }
            FARPROC fn2 = GetProcAddress(module, "DirectSoundCreate");
            if (fn2) {
                ds.setOriginalCreate(reinterpret_cast<void *>(fn2));
                KRKR_LOG_INFO(std::string("Captured DirectSoundCreate from newly loaded module: ") + nameAnsi);
            }
        }

        // FMOD
        if (strstr(nameAnsi, "fmod") || strstr(nameAnsi, "FMOD")) {
            krkrspeed::FMODHook::instance().initialize();
        }
        // Wwise
        if (strstr(nameAnsi, "AkSoundEngine") || strstr(nameAnsi, "aksoundengine")) {
             krkrspeed::WwiseHook::instance().initialize();
        }
    }

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
        if (_stricmp(procName, "DirectSoundCreate") == 0) {
            auto &ds = krkrspeed::DirectSoundHook::instance();
            ds.setOriginalCreate(reinterpret_cast<void *>(fn));
            return reinterpret_cast<FARPROC>(&krkrspeed::DirectSoundHook::DirectSoundCreateHook);
        }
        if (_stricmp(procName, "FMOD_System_PlaySound") == 0) {
            auto &fh = krkrspeed::FMODHook::instance();
            fh.setOriginalSystemPlaySound(reinterpret_cast<void*>(fn));
            // Ensure FMOD functions are loaded so we can create DSPs later
            fh.initialize();
            return reinterpret_cast<FARPROC>(krkrspeed::FMODHook::getSystemPlaySoundHook());
        }
        if (_stricmp(procName, "FMOD_Channel_SetCallback") == 0) {
            auto &fh = krkrspeed::FMODHook::instance();
            fh.setOriginalChannelSetCallback(reinterpret_cast<void*>(fn));
            return reinterpret_cast<FARPROC>(krkrspeed::FMODHook::getChannelSetCallbackHook());
        }
        return fn;
    }

    HMODULE WINAPI LoadLibraryAHook(LPCSTR lpLibFileName) {
        HMODULE h = g_origLoadLibraryA ? g_origLoadLibraryA(lpLibFileName) : nullptr;
        if (h && lpLibFileName) {
            OnLibraryLoaded(h, lpLibFileName);
        }
        return h;
    }

    HMODULE WINAPI LoadLibraryWHook(LPCWSTR lpLibFileName) {
        HMODULE h = g_origLoadLibraryW ? g_origLoadLibraryW(lpLibFileName) : nullptr;
        if (h && lpLibFileName) {
            char name[260] = {};
            WideCharToMultiByte(CP_ACP, 0, lpLibFileName, -1, name, static_cast<int>(std::size(name)), nullptr, nullptr);
            OnLibraryLoaded(h, name);
        }
        return h;
    }

    HMODULE WINAPI LoadLibraryExAHook(LPCSTR lpLibFileName, HANDLE file, DWORD flags) {
        HMODULE h = g_origLoadLibraryExA ? g_origLoadLibraryExA(lpLibFileName, file, flags) : nullptr;
        if (h && lpLibFileName) {
            OnLibraryLoaded(h, lpLibFileName);
        }
        return h;
    }

    HMODULE WINAPI LoadLibraryExWHook(LPCWSTR lpLibFileName, HANDLE file, DWORD flags) {
        HMODULE h = g_origLoadLibraryExW ? g_origLoadLibraryExW(lpLibFileName, file, flags) : nullptr;
        if (h && lpLibFileName) {
            char name[260] = {};
            WideCharToMultiByte(CP_ACP, 0, lpLibFileName, -1, name, static_cast<int>(std::size(name)), nullptr, nullptr);
            OnLibraryLoaded(h, name);
        }
        return h;
    }

    // ntdll!LdrRegisterDllNotification types
    typedef VOID(WINAPI *PFN_LdrDllNotification)(ULONG, const KRKR_LDR_DLL_NOTIFICATION_DATA *, PVOID);
    typedef NTSTATUS(WINAPI *PFN_LdrRegisterDllNotification)(ULONG, PFN_LdrDllNotification, PVOID, PVOID *);
    typedef NTSTATUS(WINAPI *PFN_LdrUnregisterDllNotification)(PVOID);
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
            std::string stage = "start";
            auto logStageFail = [&stage]() {
                KRKR_LOG_ERROR("Hook initialization thread crashed at stage: " + stage);
            };
            try {
                stage = "prolog";
                KRKR_LOG_INFO("krkr_speed_hook.dll attached; starting hook initialization");

                // Read shared settings from controller (if present).
                krkrspeed::SharedSettings shared{};
                bool haveShared = false;
                const auto mapName = krkrspeed::BuildSharedSettingsName(static_cast<std::uint32_t>(GetCurrentProcessId()));
                HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, mapName.c_str());
                if (mapping) {
                    auto *view = static_cast<krkrspeed::SharedSettings *>(
                        MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(krkrspeed::SharedSettings)));
                    if (view) {
                        shared = *view;
                        haveShared = true;
                        UnmapViewOfFile(view);
                    }
                    CloseHandle(mapping);
                }

                if (shared.enableLog) {
                    krkrspeed::SetLoggingEnabled(true);
                }
                if (shared.safeMode) {
                    KRKR_LOG_INFO("Safe mode set by controller; skipping all hooks and patches");
                    return;
                }

                const bool skipXa = haveShared ? (shared.skipXAudio2 != 0) : false;
                const bool skipDs = haveShared ? (shared.skipDirectSound != 0) : false;
                const bool skipFmod = haveShared ? (shared.skipFmod != 0) : false;
                const bool skipWwise = haveShared ? (shared.skipWwise != 0) : false;
                stage = "patch GetProcAddress";
                if (krkrspeed::PatchImport("kernel32.dll", "GetProcAddress",
                                           reinterpret_cast<void *>(&GetProcAddressHook),
                                           reinterpret_cast<void **>(&g_origGetProcAddress))) {
                    KRKR_LOG_INFO("Patched GetProcAddress import to catch dynamic audio API resolution");
                } else {
                    KRKR_LOG_WARN("Failed to patch GetProcAddress; dynamic API resolution may bypass hooks");
                }
                stage = "patch LoadLibraryA";
                if (krkrspeed::PatchImport("kernel32.dll", "LoadLibraryA",
                                           reinterpret_cast<void *>(&LoadLibraryAHook),
                                           reinterpret_cast<void **>(&g_origLoadLibraryA))) {
                    KRKR_LOG_INFO("Patched LoadLibraryA to capture late XAudio2/DirectSound loads");
                } else {
                    KRKR_LOG_WARN("Failed to patch LoadLibraryA; late-load modules may be missed");
                }
                stage = "patch LoadLibraryW";
                if (krkrspeed::PatchImport("kernel32.dll", "LoadLibraryW",
                                           reinterpret_cast<void *>(&LoadLibraryWHook),
                                           reinterpret_cast<void **>(&g_origLoadLibraryW))) {
                    KRKR_LOG_INFO("Patched LoadLibraryW to capture late XAudio2/DirectSound loads");
                } else {
                    KRKR_LOG_WARN("Failed to patch LoadLibraryW; late-load modules may be missed");
                }
                stage = "patch kernelbase LoadLibrary*";
                krkrspeed::PatchImport("kernelbase.dll", "LoadLibraryW",
                                       reinterpret_cast<void *>(&LoadLibraryWHook),
                                       reinterpret_cast<void **>(&g_origLoadLibraryW));
                krkrspeed::PatchImport("kernelbase.dll", "LoadLibraryA",
                                       reinterpret_cast<void *>(&LoadLibraryAHook),
                                       reinterpret_cast<void **>(&g_origLoadLibraryA));
                krkrspeed::PatchImport("kernelbase.dll", "LoadLibraryExW",
                                       reinterpret_cast<void *>(&LoadLibraryExWHook),
                                       reinterpret_cast<void **>(&g_origLoadLibraryExW));
                krkrspeed::PatchImport("kernelbase.dll", "LoadLibraryExA",
                                       reinterpret_cast<void *>(&LoadLibraryExAHook),
                                       reinterpret_cast<void **>(&g_origLoadLibraryExA));
                krkrspeed::PatchImport("kernel32.dll", "LoadLibraryExW",
                                       reinterpret_cast<void *>(&LoadLibraryExWHook),
                                       reinterpret_cast<void **>(&g_origLoadLibraryExW));
                krkrspeed::PatchImport("kernel32.dll", "LoadLibraryExA",
                                       reinterpret_cast<void *>(&LoadLibraryExAHook),
                                       reinterpret_cast<void **>(&g_origLoadLibraryExA));

                stage = "vectored handler";
                // Lightweight crash breadcrumbs: log first few exceptions with addresses (can be disabled).
                g_vectored = AddVectoredExceptionHandler(TRUE, VectoredHandler);

                stage = "module dump";
                try {
                    // Dump loaded module names for diagnostics.
                    HMODULE mods[256];
                    DWORD needed = 0;
                    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
                        size_t count = std::min<std::size_t>(needed / sizeof(HMODULE), std::size(mods));
                        std::string msg = "Modules:";
                        char name[MAX_PATH] = {};
                        for (size_t i = 0; i < count; ++i) {
                            if (GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, static_cast<DWORD>(std::size(name))) != 0) {
                                msg += " ";
                                msg += name;
                            }
                        }
                        KRKR_LOG_INFO(msg);
                    } else {
                        KRKR_LOG_WARN("EnumProcessModules failed; skipping module dump");
                    }
                } catch (...) {
                    KRKR_LOG_ERROR("Init: module dump threw an exception; continuing");
                }

                stage = "xaudio2 init";
                if (haveShared) {
                    krkrspeed::XAudio2Hook::instance().configureLengthGate(shared.lengthGateEnabled != 0,
                                                                          shared.lengthGateSeconds);
                }
                krkrspeed::XAudio2Hook::instance().setSkip(skipXa);
                if (!skipXa) {
                    KRKR_LOG_INFO("Init: starting XAudio2Hook::initialize");
                    try {
                        krkrspeed::XAudio2Hook::instance().initialize();
                    } catch (...) {
                        KRKR_LOG_ERROR("Init: XAudio2Hook::initialize threw an exception");
                    }
                } else {
                    KRKR_LOG_INFO("KRKR_SKIP_XAUDIO2 set; skipping XAudio2 hooks");
                }

                stage = "directsound init";
                krkrspeed::DirectSoundHook::Config dsCfg;
                dsCfg.skip = skipDs;
                dsCfg.disableBgm = haveShared ? (shared.disableBgm != 0) : false;
                dsCfg.processAllAudio = haveShared ? (shared.processAllAudio != 0) : false;
                dsCfg.bgmGateSeconds = haveShared ? shared.bgmSecondsGate : 60.0f;
                dsCfg.stereoBgmMode = haveShared ? shared.stereoBgmMode : 1u;
                krkrspeed::DirectSoundHook::instance().configure(dsCfg);

                KRKR_LOG_INFO("Init: starting DirectSoundHook::initialize");
                try {
                    krkrspeed::DirectSoundHook::instance().initialize();
                } catch (...) {
                    KRKR_LOG_ERROR("Init: DirectSoundHook::initialize threw an exception");
                }

                stage = "fmod init";
                KRKR_LOG_INFO("Init: starting FMODHook::initialize");
                try {
                    if (!skipFmod) {
                        krkrspeed::FMODHook::instance().initialize();
                    } else {
                        KRKR_LOG_INFO("KRKR_SKIP_FMOD set; skipping FMOD hooks");
                    }
                } catch (...) {
                    KRKR_LOG_ERROR("Init: FMODHook::initialize threw an exception");
                }

                stage = "wwise init";
                KRKR_LOG_INFO("Init: starting WwiseHook::initialize");
                try {
                    if (!skipWwise) {
                        krkrspeed::WwiseHook::instance().initialize();
                    } else {
                        KRKR_LOG_INFO("KRKR_SKIP_WWISE set; skipping Wwise hooks");
                    }
                } catch (...) {
                    KRKR_LOG_ERROR("Init: WwiseHook::initialize threw an exception");
                }

                stage = "ldr notify";
                // Optionally skip LdrRegisterDllNotification if requested (troubleshooting).
                KRKR_LOG_INFO("Init: registering LdrRegisterDllNotification");
                try {
                    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
                    auto reg = reinterpret_cast<PFN_LdrRegisterDllNotification>(
                        GetProcAddress(ntdll, "LdrRegisterDllNotification"));
                    if (reg) {
                        NTSTATUS st = reg(0, [](ULONG reason, const KRKR_LDR_DLL_NOTIFICATION_DATA *data, PVOID) {
                            if (reason != 1 || !data || !data->Loaded.FullDllName || !data->Loaded.FullDllName->Buffer) return;
                            const auto *full = data->Loaded.FullDllName;
                            std::wstring wname(full->Buffer, full->Length / sizeof(wchar_t));
                            if (!wname.empty()) {
                                char name[260] = {};
                                WideCharToMultiByte(CP_ACP, 0, wname.c_str(), -1, name, static_cast<int>(std::size(name)), nullptr, nullptr);
                                OnLibraryLoaded(reinterpret_cast<HMODULE>(data->Loaded.DllBase), name);
                            }
                        }, nullptr, &g_dllNotifyCookie);
                        if (st != 0) {
                            KRKR_LOG_WARN("LdrRegisterDllNotification returned status " + std::to_string(st));
                        } else {
                            KRKR_LOG_INFO("Init: LdrRegisterDllNotification registered");
                        }
                    } else {
                        KRKR_LOG_WARN("Init: LdrRegisterDllNotification not found");
                    }
                } catch (...) {
                    KRKR_LOG_ERROR("Init: LdrRegisterDllNotification threw an exception");
                }

                stage = "done";
                KRKR_LOG_INFO("Hook initialization thread finished");
            } catch (...) {
                logStageFail();
            }
        }).detach();
    }
    return TRUE;
}
