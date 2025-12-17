#include "ControllerCore.h"
#include "../common/SharedSettings.h"
#include "../common/Logging.h"
#include <TlHelp32.h>
#include <Psapi.h>
#include <shellapi.h>
#include <system_error>
#include <fstream>
#include <algorithm>
#include <unordered_map>
#include <mutex>

namespace krkrspeed::controller {
namespace {

ProcessArch classifyMachine(USHORT machine) {
    switch (machine) {
    case IMAGE_FILE_MACHINE_I386: return ProcessArch::X86;
    case IMAGE_FILE_MACHINE_AMD64: return ProcessArch::X64;
    case IMAGE_FILE_MACHINE_ARM64: return ProcessArch::Arm64;
    default: return ProcessArch::Unknown;
    }
}

bool hasVisibleWindow(DWORD pid) {
    struct EnumData {
        DWORD pid;
        bool found = false;
    } data{pid, false};

    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            auto *d = reinterpret_cast<EnumData *>(lParam);
            DWORD windowPid = 0;
            GetWindowThreadProcessId(hwnd, &windowPid);
            if (windowPid == d->pid && IsWindowVisible(hwnd)) {
                d->found = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&data));
    return data.found;
}

std::filesystem::path getProcessDirectory(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return {};
    }
    wchar_t buffer[MAX_PATH] = {};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (QueryFullProcessImageNameW(process, 0, buffer, &size) == 0) {
        CloseHandle(process);
        return {};
    }
    CloseHandle(process);
    std::filesystem::path exePath(buffer);
    return exePath.parent_path();
}

std::filesystem::path findInjectorForArch(const std::filesystem::path &controllerDir, ProcessArch arch) {
    const auto siblingX86 = controllerDir.parent_path() / L"x86";
    const auto siblingX64 = controllerDir.parent_path() / L"x64";
    std::vector<std::filesystem::path> candidates;
    if (arch == ProcessArch::X86) {
        candidates = {
            controllerDir / L"x86" / L"krkr_injector.exe",
            siblingX86 / L"krkr_injector.exe",
            controllerDir / L"krkr_injector.exe"
        };
    } else if (arch == ProcessArch::X64) {
        candidates = {
            controllerDir / L"x64" / L"krkr_injector.exe",
            siblingX64 / L"krkr_injector.exe",
            controllerDir / L"krkr_injector.exe"
        };
    }
    for (const auto &c : candidates) {
        if (!c.empty() && std::filesystem::exists(c)) return c;
    }
    return {};
}

bool runInjector(const std::filesystem::path &injector, DWORD pid, const std::filesystem::path &dllPath,
                 std::wstring &error) {
    if (injector.empty()) {
        error = L"Injector executable not found for target architecture.";
        return false;
    }

    std::wstring args = L"\"" + injector.wstring() + L"\" " + std::to_wstring(pid) + L" \"" + dllPath.wstring() + L"\"";
    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    BOOL ok = CreateProcessW(nullptr, args.data(), nullptr, nullptr, FALSE, 0, nullptr,
                             injector.parent_path().c_str(), &si, &pi);
    if (!ok) {
        error = L"CreateProcess for injector failed: " + std::to_wstring(GetLastError());
        return false;
    }
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (exitCode != 0) {
        error = L"Injector exit code " + std::to_wstring(exitCode);
        return false;
    }
    return true;
}

std::vector<std::wstring> buildShortAndLongPaths(const std::filesystem::path &p) {
    std::vector<std::wstring> out;
    if (!p.empty()) out.push_back(p.wstring());
    wchar_t shortBuf[MAX_PATH] = {};
    DWORD shortLen = GetShortPathNameW(p.c_str(), shortBuf, static_cast<DWORD>(std::size(shortBuf)));
    if (shortLen > 0 && shortLen < std::size(shortBuf)) {
        out.emplace_back(shortBuf, shortLen);
    }
    return out;
}

// Keep shared setting mappings alive so the target process can open them after we return.
std::unordered_map<DWORD, HANDLE> g_sharedMappings;
std::mutex g_sharedMutex;

} // namespace

ProcessArch getSelfArch() {
#if defined(_M_X64)
    return ProcessArch::X64;
#elif defined(_M_IX86)
    return ProcessArch::X86;
#elif defined(_M_ARM64)
    return ProcessArch::Arm64;
#else
    return ProcessArch::Unknown;
#endif
}

std::wstring describeArch(ProcessArch arch) {
    switch (arch) {
    case ProcessArch::X86: return L"x86";
    case ProcessArch::X64: return L"x64";
    case ProcessArch::Arm64: return L"ARM64";
    default: return L"unknown";
    }
}

bool ensureDebugPrivilege() {
    static bool attempted = false;
    static bool enabled = false;
    if (attempted) return enabled;
    attempted = true;

    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        KRKR_LOG_WARN("OpenProcessToken failed while enabling SeDebugPrivilege: " + std::to_string(GetLastError()));
        return false;
    }

    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &luid)) {
        KRKR_LOG_WARN("LookupPrivilegeValueW(SE_DEBUG_NAME) failed: " + std::to_string(GetLastError()));
        CloseHandle(token);
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr)) {
        KRKR_LOG_WARN("AdjustTokenPrivileges failed: " + std::to_string(GetLastError()));
        CloseHandle(token);
        return false;
    }
    const DWORD lastErr = GetLastError();
    CloseHandle(token);
    if (lastErr == ERROR_NOT_ALL_ASSIGNED) {
        KRKR_LOG_WARN("SeDebugPrivilege not assigned; elevated/protected targets may still reject injection");
        return false;
    }

    enabled = true;
    KRKR_LOG_INFO("SeDebugPrivilege enabled for injector process");
    return true;
}

std::filesystem::path controllerDirectory() {
    wchar_t buffer[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) == 0) {
        return {};
    }
    std::filesystem::path path(buffer);
    return path.parent_path();
}

std::vector<ProcessInfo> enumerateVisibleProcesses() {
    std::vector<ProcessInfo> result;

    DWORD currentSession = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &currentSession)) {
        KRKR_LOG_WARN("ProcessIdToSessionId failed for current process; defaulting to no session filter");
        currentSession = (std::numeric_limits<DWORD>::max)();
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        KRKR_LOG_ERROR("CreateToolhelp32Snapshot failed: " + std::to_string(GetLastError()));
        return result;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            DWORD session = 0;
            if (!ProcessIdToSessionId(entry.th32ProcessID, &session)) {
                continue;
            }
            if (session == 0 || session != currentSession) {
                continue; // skip services and other sessions
            }
            if (!hasVisibleWindow(entry.th32ProcessID)) {
                continue; // skip helpers/subprocesses without a top-level window
            }

            ProcessInfo info;
            info.name = entry.szExeFile;
            info.pid = entry.th32ProcessID;
            info.hasWindow = true;
            result.push_back(std::move(info));
        } while (Process32NextW(snapshot, &entry));
    } else {
        KRKR_LOG_ERROR("Process32FirstW failed: " + std::to_string(GetLastError()));
    }

    CloseHandle(snapshot);
    std::sort(result.begin(), result.end(), [](const ProcessInfo &a, const ProcessInfo &b) {
        return a.name < b.name;
    });
    return result;
}

bool queryProcessArch(DWORD pid, ProcessArch &archOut, std::wstring &error) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        error = L"OpenProcess failed while probing architecture (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    using IsWow64Process2Fn = BOOL(WINAPI *)(HANDLE, USHORT *, USHORT *);
    auto isWow64Process2 = reinterpret_cast<IsWow64Process2Fn>(GetProcAddress(kernel, "IsWow64Process2"));
    if (isWow64Process2) {
        USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        if (isWow64Process2(process, &processMachine, &nativeMachine)) {
            archOut = classifyMachine(processMachine == IMAGE_FILE_MACHINE_UNKNOWN ? nativeMachine : processMachine);
            CloseHandle(process);
            return true;
        }
    }

    BOOL wow64 = FALSE;
    if (IsWow64Process(process, &wow64)) {
        SYSTEM_INFO info{};
        GetNativeSystemInfo(&info);
        if (wow64) {
            archOut = ProcessArch::X86;
        } else {
            switch (info.wProcessorArchitecture) {
            case PROCESSOR_ARCHITECTURE_AMD64: archOut = ProcessArch::X64; break;
            case PROCESSOR_ARCHITECTURE_ARM64: archOut = ProcessArch::Arm64; break;
            default: archOut = ProcessArch::Unknown; break;
            }
        }
        CloseHandle(process);
        return true;
    }

    error = L"Unable to query process architecture (error " + std::to_wstring(GetLastError()) + L")";
    CloseHandle(process);
    return false;
}

bool getDllArch(const std::filesystem::path &path, ProcessArch &archOut, std::wstring &error) {
    archOut = ProcessArch::Unknown;
    if (!std::filesystem::exists(path)) {
        error = L"Hook DLL not found: " + path.wstring();
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = L"Unable to open DLL to read architecture: " + path.wstring();
        return false;
    }

    std::uint8_t header[0x44] = {};
    file.read(reinterpret_cast<char *>(header), sizeof(header));
    if (file.gcount() < 0x44) {
        error = L"Unable to read DLL headers for: " + path.wstring();
        return false;
    }
    const std::uint32_t peOffset = *reinterpret_cast<const std::uint32_t *>(header + 0x3c);
    file.seekg(peOffset + 4, std::ios::beg);
    std::uint16_t machine = 0;
    file.read(reinterpret_cast<char *>(&machine), sizeof(machine));
    if (file.gcount() != sizeof(machine)) {
        error = L"Unable to read DLL machine type: " + path.wstring();
        return false;
    }

    archOut = classifyMachine(machine);
    return true;
}

bool selectHookForArch(const std::filesystem::path &controllerDir, ProcessArch arch, std::filesystem::path &outPath,
                       std::wstring &error) {
    const auto siblingX86 = controllerDir.parent_path() / L"x86";
    const auto siblingX64 = controllerDir.parent_path() / L"x64";
    std::vector<std::filesystem::path> candidates;
    if (arch == ProcessArch::X86) {
        candidates = {
            controllerDir / L"x86" / L"krkr_speed_hook.dll",
            siblingX86 / L"krkr_speed_hook.dll",
            controllerDir / L"krkr_speed_hook32.dll",
            controllerDir / L"krkr_speed_hook_x86.dll",
            controllerDir / L"krkr_speed_hook.dll"
        };
    } else {
        candidates = {
            controllerDir / L"x64" / L"krkr_speed_hook.dll",
            siblingX64 / L"krkr_speed_hook.dll",
            controllerDir / L"krkr_speed_hook64.dll",
            controllerDir / L"krkr_speed_hook_x64.dll",
            controllerDir / L"krkr_speed_hook.dll"
        };
    }

    for (const auto &candidate : candidates) {
        if (!candidate.empty() && std::filesystem::exists(candidate)) {
            outPath = std::filesystem::absolute(candidate);
            return true;
        }
    }

    if (arch == ProcessArch::X86) {
        error = L"Matching hook DLL not found. Place x86/krkr_speed_hook.dll (or krkr_speed_hook32.dll) next to the controller.";
    } else {
        error = L"Matching hook DLL not found. Place x64/krkr_speed_hook.dll (or krkr_speed_hook64.dll) next to the controller.";
    }
    return false;
}

bool writeSharedSettingsForPid(DWORD pid, const SharedConfig &config, std::wstring &error) {
    const auto name = BuildSharedSettingsName(static_cast<std::uint32_t>(pid));

    HANDLE mapping = nullptr;
    auto recreateMapping = [&]() -> HANDLE {
        return CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedSettings), name.c_str());
    };

    {
        std::lock_guard<std::mutex> lock(g_sharedMutex);
        auto it = g_sharedMappings.find(pid);
        if (it != g_sharedMappings.end()) {
            mapping = it->second;
        }
    }
    if (!mapping) {
        mapping = recreateMapping();
    }
    if (!mapping) {
        error = L"CreateFileMapping failed: " + std::to_wstring(GetLastError());
        return false;
    }

    auto mapView = [&](HANDLE h) -> SharedSettings * {
        return static_cast<SharedSettings *>(MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, sizeof(SharedSettings)));
    };

    SharedSettings *view = mapView(mapping);
    if (!view && GetLastError() == ERROR_INVALID_HANDLE) {
        // Handle likely closed/invalid; recreate once.
        mapping = recreateMapping();
        if (mapping) {
            view = mapView(mapping);
        }
    }
    if (!view) {
        error = L"MapViewOfFile failed: " + std::to_wstring(GetLastError());
        return false;
    }

    SharedSettings settings{};
    settings.userSpeed = config.speed;
    settings.lengthGateSeconds = std::clamp(config.bgmSeconds, 0.1f, 600.0f);
    settings.lengthGateEnabled = config.lengthGateEnabled ? 1u : 0u;
    settings.enableLog = config.enableLog ? 1u : 0u;
    settings.skipDirectSound = config.skipDirectSound ? 1u : 0u;
    settings.skipXAudio2 = config.skipXAudio2 ? 1u : 0u;
    settings.skipFmod = config.skipFmod ? 1u : 0u;
    settings.skipWwise = config.skipWwise ? 1u : 0u;
    settings.safeMode = config.safeMode ? 1u : 0u;
    settings.disableBgm = 0;
    settings.processAllAudio = config.processAllAudio ? 1u : 0u;
    settings.bgmSecondsGate = std::clamp(config.bgmSeconds, 0.1f, 600.0f);
    settings.stereoBgmMode = config.stereoBgmMode;
    *view = settings;

    UnmapViewOfFile(view);
    {
        std::lock_guard<std::mutex> lock(g_sharedMutex);
        auto it = g_sharedMappings.find(pid);
        if (it != g_sharedMappings.end() && it->second != mapping) {
            CloseHandle(it->second);
        }
        g_sharedMappings[pid] = mapping; // keep alive for hook to open
    }
    return true;
}

bool injectDllIntoProcess(ProcessArch targetArch, DWORD pid, const std::filesystem::path &dllPath, std::wstring &error) {
    ensureDebugPrivilege();

    const ProcessArch selfArch = getSelfArch();
    const bool archMismatch = (targetArch != ProcessArch::Unknown && selfArch != ProcessArch::Unknown && targetArch != selfArch);
    if (!archMismatch) {
        auto preflightLoad = [](const std::filesystem::path &path, std::wstring &err) -> bool {
            SetEnvironmentVariableW(L"KRKR_SKIP_HOOK_INIT", L"1");
            HMODULE localHandle = LoadLibraryW(path.c_str());
            SetEnvironmentVariableW(L"KRKR_SKIP_HOOK_INIT", nullptr);
            if (!localHandle) {
                err = L"Preflight LoadLibraryW failed locally (error " + std::to_wstring(GetLastError()) +
                      L"); check dependencies beside the DLL.";
                return false;
            }
            FreeLibrary(localHandle);
            return true;
        };

        std::wstring preflightErr;
        if (!preflightLoad(dllPath, preflightErr)) {
            error = preflightErr;
            return false;
        }
    } else {
        KRKR_LOG_INFO("Skipping preflight load due to arch mismatch controller=" + std::string(selfArch == ProcessArch::X64 ? "x64" : "x86")
                      + " target=" + std::string(targetArch == ProcessArch::X64 ? "x64" : "x86"));
    }

    std::vector<std::wstring> attemptNotes;
    std::wstring lastError;
    size_t attemptIdx = 0;
    const auto controllerDir = controllerDirectory();
    const auto injector = findInjectorForArch(controllerDir, targetArch);

    for (const auto &p : buildShortAndLongPaths(dllPath)) {
        if (p.empty()) continue;
        if (runInjector(injector, pid, p, lastError)) {
            return true;
        }
        attemptNotes.push_back(L"#" + std::to_wstring(attemptIdx) + L" " + p + L": " + lastError);
        ++attemptIdx;
    }

    const auto targetDir = getProcessDirectory(pid);
    if (!targetDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(targetDir, ec);
        const auto targetDll = targetDir / dllPath.filename();
        const auto dep = dllPath.parent_path() / L"SoundTouch.dll";
        bool copied = false;
        if (std::filesystem::exists(dllPath)) {
            std::filesystem::copy_file(dllPath, targetDll, std::filesystem::copy_options::overwrite_existing, ec);
            copied = !ec;
            if (ec) {
                attemptNotes.push_back(L"Copy to target dir failed for hook: " + targetDll.wstring() +
                                       L" (error " + std::to_wstring(ec.value()) + L")");
            }
        }
        if (std::filesystem::exists(dep)) {
            auto targetDep = targetDir / dep.filename();
            std::filesystem::copy_file(dep, targetDep, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                attemptNotes.push_back(L"Copy to target dir failed for SoundTouch: " + targetDep.wstring() +
                                       L" (error " + std::to_wstring(ec.value()) + L")");
            }
        }
        if (copied) {
            for (const auto &p : buildShortAndLongPaths(targetDll)) {
                if (p.empty()) continue;
                if (runInjector(injector, pid, p, lastError)) {
                    return true;
                }
                attemptNotes.push_back(L"#" + std::to_wstring(attemptIdx) + L" " + p + L": " + lastError);
                ++attemptIdx;
            }
        }
    }

    error = L"DLL injection returned 0 (remote LoadLibraryW failed). ";
    if (!attemptNotes.empty()) {
        error += L"Attempts: ";
        for (size_t i = 0; i < attemptNotes.size(); ++i) {
            if (i) error += L" | ";
            error += attemptNotes[i];
        }
    } else {
        error += L"Tried path: " + dllPath.wstring();
    }
    if (!targetDir.empty()) {
        error += L"; target dir: " + targetDir.wstring();
    }
    if (!lastError.empty()) {
        error += L" Last attempt: " + lastError;
    }
    return false;
}

bool launchAndInject(const std::filesystem::path &exePath, const SharedConfig &config, DWORD &outPid, std::wstring &error) {
    if (exePath.empty() || !std::filesystem::exists(exePath)) {
        error = L"Invalid game path";
        return false;
    }
    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    std::wstring cmd = L"\"" + exePath.wstring() + L"\"";
    auto workDir = exePath.parent_path();
    BOOL ok = CreateProcessW(exePath.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                             CREATE_SUSPENDED, nullptr,
                             workDir.empty() ? nullptr : workDir.c_str(),
                             &si, &pi);
    if (!ok) {
        error = L"CreateProcess failed: " + std::to_wstring(GetLastError());
        return false;
    }

    outPid = pi.dwProcessId;

    ProcessArch targetArch = ProcessArch::Unknown;
    std::wstring archError;
    if (!queryProcessArch(pi.dwProcessId, targetArch, archError)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        error = archError;
        return false;
    }

    std::wstring sharedErr;
    if (!writeSharedSettingsForPid(pi.dwProcessId, config, sharedErr)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        error = sharedErr;
        return false;
    }

    const auto baseDir = controllerDirectory();
    std::filesystem::path dllPath;
    std::wstring archErr;
    if (!selectHookForArch(baseDir, targetArch, dllPath, archErr)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        error = archErr;
        return false;
    }

    ProcessArch dllArch = ProcessArch::Unknown;
    std::wstring dllArchError;
    if (!getDllArch(dllPath, dllArch, dllArchError) || (dllArch != ProcessArch::Unknown && dllArch != targetArch)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        error = L"Hook DLL arch mismatch";
        return false;
    }

    std::wstring injectErr;
    if (!injectDllIntoProcess(targetArch, pi.dwProcessId, dllPath, injectErr)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        error = L"Launch inject failed: " + injectErr;
        return false;
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace krkrspeed::controller
