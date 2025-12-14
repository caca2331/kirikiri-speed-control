#ifndef NOMINMAX
#define NOMINMAX 1
#endif

#include "ui.h"

#include "../common/Logging.h"
#include "../hook/XAudio2Hook.h"
#include "../common/SharedSettings.h"

#include <Windows.h>
#include <CommCtrl.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <system_error>
#include <string>
#include <vector>

namespace krkrspeed::ui {
namespace {

constexpr int kProcessComboId = 1001;
constexpr int kRefreshButtonId = 1002;
constexpr int kSpeedEditId = 1003;
constexpr int kApplyButtonId = 1004;
constexpr int kStatusLabelId = 1005;
constexpr int kLinkId = 1006;
constexpr int kPathEditId = 1007;
constexpr int kLaunchButtonId = 1008;
constexpr int kIgnoreBgmCheckId = 1009;
constexpr UINT kMsgRefreshQuiet = WM_APP + 1;

struct ProcessInfo {
    std::wstring name;
    DWORD pid = 0;
};

struct AppState {
    std::vector<ProcessInfo> processes;
    float currentSpeed = 2.0f;
    float lastValidSpeed = 2.0f;
    float gateSeconds = 60.0f;
    std::filesystem::path launchPath;
    bool enableLog = false;
    bool skipDirectSound = false;
    bool skipXAudio2 = false;
    bool safeMode = false;
    bool disableVeh = false;
    bool disableBgm = false;
    bool forceAll = false;
    float bgmSeconds = 60.0f;
    std::vector<std::wstring> tooltipTexts; // keep strings alive for tooltips
    std::map<DWORD, HANDLE> sharedMaps;
    std::map<DWORD, krkrspeed::SharedSettings *> sharedViews;
    std::uint32_t stereoBgmMode = 1;
};

AppState g_state;
static HWND g_link = nullptr;
static ControllerOptions g_initialOptions{};

enum class ProcessArch {
    Unknown,
    X86,
    X64,
    Arm64
};

ProcessArch classifyMachine(USHORT machine) {
    switch (machine) {
    case IMAGE_FILE_MACHINE_I386:
        return ProcessArch::X86;
    case IMAGE_FILE_MACHINE_AMD64:
        return ProcessArch::X64;
    case IMAGE_FILE_MACHINE_ARM64:
        return ProcessArch::Arm64;
    default:
        return ProcessArch::Unknown;
    }
}

std::wstring describeArch(ProcessArch arch) {
    switch (arch) {
    case ProcessArch::X86:
        return L"x86";
    case ProcessArch::X64:
        return L"x64";
    case ProcessArch::Arm64:
        return L"ARM64";
    default:
        return L"unknown";
    }
}

bool ensureDebugPrivilege() {
    static bool attempted = false;
    static bool enabled = false;
    if (attempted) {
        return enabled;
    }
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

constexpr ProcessArch getSelfArch() {
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
            case PROCESSOR_ARCHITECTURE_AMD64:
                archOut = ProcessArch::X64;
                break;
            case PROCESSOR_ARCHITECTURE_ARM64:
                archOut = ProcessArch::Arm64;
                break;
            default:
                archOut = ProcessArch::Unknown;
                break;
            }
        }
        CloseHandle(process);
        return true;
    }

    error = L"Unable to query process architecture (error " + std::to_wstring(GetLastError()) + L")";
    CloseHandle(process);
    return false;
}

std::filesystem::path getExecutableDir() {
    wchar_t buffer[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) == 0) {
        return {};
    }
    std::filesystem::path path(buffer);
    return path.parent_path();
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

std::vector<ProcessInfo> enumerateProcesses() {
    std::vector<ProcessInfo> result;

    DWORD currentSession = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &currentSession)) {
        KRKR_LOG_WARN("ProcessIdToSessionId failed for current process; defaulting to no session filter");
        currentSession = std::numeric_limits<DWORD>::max();
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
                continue; // skip background helpers/subprocesses without a top-level window
            }

            ProcessInfo info;
            info.name = entry.szExeFile;
            info.pid = entry.th32ProcessID;
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

void setStatus(HWND statusLabel, const std::wstring &text) {
    SetWindowTextW(statusLabel, text.c_str());
    KRKR_LOG_INFO(text);
}

float readSpeedFromEdit(HWND edit) {
    wchar_t buffer[32] = {};
    GetWindowTextW(edit, buffer, static_cast<int>(std::size(buffer)));
    wchar_t *end = nullptr;
    float parsed = std::wcstof(buffer, &end);
    if (end == buffer || !std::isfinite(parsed)) {
        // Restore previous valid value.
        wchar_t normalized[32] = {};
        swprintf_s(normalized, L"%.2f", g_state.lastValidSpeed);
        SetWindowTextW(edit, normalized);
        return g_state.lastValidSpeed;
    }
    if (parsed < 0.5f || parsed > 10.0f) {
        wchar_t normalized[32] = {};
        swprintf_s(normalized, L"%.2f", g_state.lastValidSpeed);
        SetWindowTextW(edit, normalized);
        return g_state.lastValidSpeed;
    }

    const float clamped = std::clamp(parsed, 0.5f, 10.0f);
    g_state.lastValidSpeed = clamped;
    return clamped;
}

void populateProcessCombo(HWND combo, const std::vector<ProcessInfo> &processes) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (const auto &proc : processes) {
        std::wstring label = L"[" + std::to_wstring(proc.pid) + L"] " + proc.name;
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    if (!processes.empty()) {
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
    }
}

std::filesystem::path getProcessDirectory(DWORD pid);
bool runInjector(ProcessArch arch, DWORD pid, const std::wstring &dllPath, std::wstring &error);
bool injectDllIntoProcess(ProcessArch targetArch, DWORD pid, const std::wstring &dllPath, std::wstring &error);

bool injectDllIntoProcess(ProcessArch targetArch, DWORD pid, const std::wstring &dllPath, std::wstring &error) {
    ensureDebugPrivilege();

    // Preflight: ensure we can load the DLL locally (catches missing dependencies) when arch matches.
    const ProcessArch selfArch = getSelfArch();
    const bool archMismatch = (targetArch != ProcessArch::Unknown && selfArch != ProcessArch::Unknown && targetArch != selfArch);
    if (!archMismatch) {
        auto preflightLoad = [](const std::wstring &path, std::wstring &err) -> bool {
            SetEnvironmentVariableW(L"KRKR_SKIP_HOOK_INIT", L"1"); // avoid patching the controller during the preflight load
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

    auto buildCandidates = [&](const std::wstring &basePath) {
        std::vector<std::wstring> out;
        if (!basePath.empty()) out.push_back(basePath);
        wchar_t shortBuf[MAX_PATH] = {};
        DWORD shortLen = GetShortPathNameW(basePath.c_str(), shortBuf, static_cast<DWORD>(std::size(shortBuf)));
        if (shortLen > 0 && shortLen < std::size(shortBuf)) {
            out.emplace_back(shortBuf, shortLen);
        }
        return out;
    };

    std::vector<std::wstring> candidates = buildCandidates(dllPath);
    std::wstring lastError;
    size_t attemptIdx = 0;
    for (const auto &p : candidates) {
        if (p.empty()) continue;
        if (runInjector(targetArch, pid, p, lastError)) {
            return true;
        }
        attemptNotes.push_back(L"#" + std::to_wstring(attemptIdx) + L" " + p + L": " + lastError);
        ++attemptIdx;
    }

    // If initial attempts failed, try copying into the target's exe directory and retry.
    const auto targetDir = getProcessDirectory(pid);
    if (!targetDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(targetDir, ec);
        const auto targetDll = targetDir / std::filesystem::path(dllPath).filename();
        const auto dep = std::filesystem::path(dllPath).parent_path() / L"SoundTouch.dll";
        bool copied = false;
        bool copiedDep = false;
        if (std::filesystem::exists(dllPath)) {
            std::filesystem::copy_file(dllPath, targetDll, std::filesystem::copy_options::overwrite_existing, ec);
            copied = !ec;
            if (ec) {
                attemptNotes.push_back(L"Copy to target dir failed for hook: " + targetDll.wstring() +
                                       L" (error " + std::to_wstring(ec.value()) + L")");
            }
        }
        std::filesystem::path targetDep;
        if (std::filesystem::exists(dep)) {
            targetDep = targetDir / dep.filename();
            std::filesystem::copy_file(dep, targetDep, std::filesystem::copy_options::overwrite_existing, ec);
            copiedDep = !ec;
            if (ec) {
                attemptNotes.push_back(L"Copy to target dir failed for SoundTouch: " + targetDep.wstring() +
                                       L" (error " + std::to_wstring(ec.value()) + L")");
            }
        }
        if (copied) {
            auto more = buildCandidates(targetDll.wstring());
            for (const auto &p : more) {
                if (p.empty()) continue;
                if (runInjector(targetArch, pid, p, lastError)) {
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
        error += L"Tried path: " + dllPath;
        if (candidates.size() > 1 && !candidates[1].empty()) {
            error += L" and short path: " + candidates[1];
        }
    }
    if (!targetDir.empty()) {
        error += L"; target dir: " + targetDir.wstring();
    }
    error += L". Ensure matching arch, DLL exists, dependencies like SoundTouch.dll are beside it, target not protected, and controller runs with needed privileges.";
    if (!lastError.empty()) {
        error += L" Last attempt: " + lastError;
    }
    return false;
}

void refreshProcessList(HWND combo, HWND statusLabel, bool quiet = false) {
    g_state.processes = enumerateProcesses();
    populateProcessCombo(combo, g_state.processes);
    if (!quiet) {
        std::wstring status = L"Found " + std::to_wstring(g_state.processes.size()) + L" processes";
        setStatus(statusLabel, status);
    }
}

bool selectHookForArch(ProcessArch arch, std::wstring &outPath, std::wstring &error) {
    const auto baseDir = getExecutableDir();
    const auto siblingX86 = baseDir.parent_path() / L"x86";
    const auto siblingX64 = baseDir.parent_path() / L"x64";
    if (baseDir.empty()) {
        error = L"Unable to locate controller directory.";
        return false;
    }

    std::vector<std::filesystem::path> candidates;
    if (arch == ProcessArch::X86) {
        candidates = {
            baseDir / L"x86" / L"krkr_speed_hook.dll",
            siblingX86 / L"krkr_speed_hook.dll",
            baseDir / L"krkr_speed_hook32.dll",
            baseDir / L"krkr_speed_hook_x86.dll",
            baseDir / L"krkr_speed_hook.dll" // fallback
        };
    } else {
        candidates = {
            baseDir / L"x64" / L"krkr_speed_hook.dll",
            siblingX64 / L"krkr_speed_hook.dll",
            baseDir / L"krkr_speed_hook64.dll",
            baseDir / L"krkr_speed_hook_x64.dll",
            baseDir / L"krkr_speed_hook.dll" // fallback
        };
    }

    for (const auto &candidate : candidates) {
        if (!candidate.empty() && std::filesystem::exists(candidate)) {
            outPath = std::filesystem::absolute(candidate).wstring();
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

std::filesystem::path findInjectorForArch(ProcessArch arch) {
    const auto baseDir = getExecutableDir();
    const auto siblingX86 = baseDir.parent_path() / L"x86";
    const auto siblingX64 = baseDir.parent_path() / L"x64";
    std::vector<std::filesystem::path> candidates;
    if (arch == ProcessArch::X86) {
        candidates = {
            baseDir / L"x86" / L"krkr_injector.exe",
            siblingX86 / L"krkr_injector.exe",
            baseDir / L"krkr_injector.exe" // fallback (may be wrong arch)
        };
    } else if (arch == ProcessArch::X64) {
        candidates = {
            baseDir / L"x64" / L"krkr_injector.exe",
            siblingX64 / L"krkr_injector.exe",
            baseDir / L"krkr_injector.exe"
        };
    }
    for (const auto &c : candidates) {
        if (!c.empty() && std::filesystem::exists(c)) {
            return std::filesystem::absolute(c);
        }
    }
    return {};
}

bool runInjector(ProcessArch arch, DWORD pid, const std::wstring &dllPath, std::wstring &error) {
    auto injector = findInjectorForArch(arch);
    if (injector.empty()) {
        error = L"Injector executable not found for arch " + describeArch(arch);
        return false;
    }

    wchar_t cmdLine[1024] = {};
    swprintf_s(cmdLine, L"\"%s\" %u \"%s\"", injector.c_str(), pid, dllPath.c_str());

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    if (!CreateProcessW(nullptr, cmdLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        error = L"CreateProcess failed for injector (" + injector.wstring() + L") error=" +
                std::to_wstring(GetLastError());
        return false;
    }
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (exitCode != 0) {
        error = L"Injector exit code " + std::to_wstring(exitCode) + L" (" + injector.wstring() + L")";
        return false;
    }
    return true;
}

bool writeSharedSettingsForPid(DWORD pid, float speed, bool gateEnabled, float duration, std::wstring &error) {
    using krkrspeed::SharedSettings;
    const auto name = krkrspeed::BuildSharedSettingsName(pid);

    HANDLE mapping = nullptr;
    auto it = g_state.sharedMaps.find(pid);
    if (it != g_state.sharedMaps.end()) {
        mapping = it->second;
    } else {
        mapping = OpenFileMappingW(FILE_MAP_WRITE, FALSE, name.c_str());
        if (!mapping) {
            mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedSettings),
                                         name.c_str());
        }
        if (!mapping) {
            error = L"Unable to create shared settings map (error " + std::to_wstring(GetLastError()) + L")";
            return false;
        }
        g_state.sharedMaps[pid] = mapping;
    }

    SharedSettings *view = nullptr;
    auto viewIt = g_state.sharedViews.find(pid);
    if (viewIt != g_state.sharedViews.end()) {
        view = viewIt->second;
    }
    if (!view) {
        view = static_cast<SharedSettings *>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(SharedSettings)));
        if (!view) {
            error = L"MapViewOfFile failed for shared settings (error " + std::to_wstring(GetLastError()) + L")";
            CloseHandle(mapping);
            g_state.sharedMaps.erase(pid);
            return false;
        }
        g_state.sharedViews[pid] = view;
    }

    SharedSettings settings;
    settings.userSpeed = std::clamp(speed, 0.5f, 10.0f);
    settings.lengthGateSeconds = std::clamp(duration, 0.1f, 600.0f);
    settings.lengthGateEnabled = gateEnabled ? 1u : 0u;
    settings.version = 2;
    settings.enableLog = g_state.enableLog ? 1u : 0u;
    settings.skipDirectSound = g_state.skipDirectSound ? 1u : 0u;
    settings.skipXAudio2 = g_state.skipXAudio2 ? 1u : 0u;
    settings.safeMode = g_state.safeMode ? 1u : 0u;
    settings.disableVeh = g_state.disableVeh ? 1u : 0u;
    settings.disableBgm = g_state.disableBgm ? 1u : 0u;
    settings.forceAll = g_state.forceAll ? 1u : 0u;
    settings.bgmSecondsGate = std::clamp(g_state.bgmSeconds, 0.1f, 600.0f);
    *view = settings;
    return true;
}

void cleanupSharedMaps() {
    for (auto &kv : g_state.sharedViews) {
        if (kv.second) {
            UnmapViewOfFile(kv.second);
        }
    }
    for (auto &kv : g_state.sharedMaps) {
        if (kv.second) {
            CloseHandle(kv.second);
        }
    }
    g_state.sharedViews.clear();
    g_state.sharedMaps.clear();
}

void handleApply(HWND hwnd) {
    HWND combo = GetDlgItem(hwnd, kProcessComboId);
    HWND editSpeed = GetDlgItem(hwnd, kSpeedEditId);
    HWND ignoreBgm = GetDlgItem(hwnd, kIgnoreBgmCheckId);
    HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);

    const float speed = readSpeedFromEdit(editSpeed);
    g_state.currentSpeed = speed;
    g_state.forceAll = (ignoreBgm && SendMessageW(ignoreBgm, BM_GETCHECK, 0, 0) == BST_CHECKED);

    krkrspeed::XAudio2Hook::instance().setUserSpeed(speed);
    krkrspeed::XAudio2Hook::instance().configureLengthGate(true, g_state.gateSeconds);

    const int index = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(g_state.processes.size())) {
        setStatus(statusLabel, L"Select a process to hook first.");
        return;
    }

    const auto &proc = g_state.processes[static_cast<std::size_t>(index)];

    ProcessArch targetArch = ProcessArch::Unknown;
    std::wstring archError;
    if (!queryProcessArch(proc.pid, targetArch, archError)) {
        setStatus(statusLabel, archError);
        return;
    }

    std::wstring sharedErr;
    if (!writeSharedSettingsForPid(proc.pid, speed, true, g_state.gateSeconds, sharedErr)) {
        setStatus(statusLabel, sharedErr);
        return;
    }

    std::wstring dllPath;
    if (!selectHookForArch(targetArch, dllPath, archError)) {
        setStatus(statusLabel, archError);
        return;
    }

    ProcessArch dllArch = ProcessArch::Unknown;
    std::wstring dllArchError;
    if (!getDllArch(dllPath, dllArch, dllArchError)) {
        setStatus(statusLabel, dllArchError);
        return;
    }
    if (targetArch != ProcessArch::Unknown && dllArch != ProcessArch::Unknown && targetArch != dllArch) {
        std::wstring msg = L"Hook DLL arch (" + describeArch(dllArch) + L") does not match target (" +
                           describeArch(targetArch) + L"). Pick the correct dist folder.";
        setStatus(statusLabel, msg);
        return;
    }

    std::wstring error;
    if (injectDllIntoProcess(targetArch, proc.pid, dllPath, error)) {
        wchar_t message[160] = {};
        swprintf_s(message, L"Injected into %s (PID %u) at %.2fx; gate on @ %.2fs",
                   proc.name.c_str(), proc.pid, speed, g_state.gateSeconds);
        setStatus(statusLabel, message);
    } else {
        constexpr ProcessArch selfArch = getSelfArch();
        std::wstring detail = L" [controller=" + describeArch(selfArch) + L", target=" + describeArch(targetArch) +
                              L", dll=" + describeArch(dllArch) + L"]";
        setStatus(statusLabel, L"Injection failed: " + error + detail);
    }
}

void handleLaunch(HWND hwnd) {
    HWND pathEdit = GetDlgItem(hwnd, kPathEditId);
    HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);
    wchar_t pathBuf[MAX_PATH * 4] = {};
    GetWindowTextW(pathEdit, pathBuf, static_cast<int>(std::size(pathBuf)));
    std::filesystem::path exePath(pathBuf);
    if (exePath.empty() || !std::filesystem::exists(exePath)) {
        setStatus(statusLabel, L"Invalid game path");
        return;
    }
    // Start suspended
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
        setStatus(statusLabel, L"CreateProcess failed: " + std::to_wstring(GetLastError()));
        return;
    }
    setStatus(statusLabel, L"Process created suspended, PID " + std::to_wstring(pi.dwProcessId));

    // Determine arch of new process
    ProcessArch targetArch = ProcessArch::Unknown;
    std::wstring archError;
    if (!queryProcessArch(pi.dwProcessId, targetArch, archError)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        setStatus(statusLabel, archError);
        return;
    }

    // Write shared settings and inject using existing flow.
    std::wstring sharedErr;
    const float speed = g_state.currentSpeed;
    if (!writeSharedSettingsForPid(pi.dwProcessId, speed, true, g_state.gateSeconds, sharedErr)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        setStatus(statusLabel, sharedErr);
        return;
    }

    std::wstring dllPath;
    std::wstring archErr;
    if (!selectHookForArch(targetArch, dllPath, archErr)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        setStatus(statusLabel, archErr);
        return;
    }

    ProcessArch dllArch = ProcessArch::Unknown;
    std::wstring dllArchError;
    if (!getDllArch(dllPath, dllArch, dllArchError) || (dllArch != ProcessArch::Unknown && dllArch != targetArch)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        setStatus(statusLabel, L"Hook DLL arch mismatch");
        return;
    }

    std::wstring injectErr;
    if (!injectDllIntoProcess(targetArch, pi.dwProcessId, dllPath, injectErr)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        setStatus(statusLabel, L"Launch inject failed: " + injectErr);
        return;
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    setStatus(statusLabel, L"Launched and injected: " + exePath.filename().wstring() +
               L" (PID " + std::to_wstring(pi.dwProcessId) + L")");
    // Refresh process list and select the new one (best-effort, 3s timeout, non-blocking UI thread).
    std::thread([hwnd, pid = pi.dwProcessId]() {
        const auto start = std::chrono::steady_clock::now();
        while (true) {
            PostMessageW(hwnd, kMsgRefreshQuiet, 0, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() > 3000) break;
            // Attempt to set selection if found.
            HWND combo = GetDlgItem(hwnd, kProcessComboId);
            if (!combo) continue;
            const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
            wchar_t item[256] = {};
            for (int i = 0; i < count; ++i) {
                if (SendMessageW(combo, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(item)) > 0) {
                    // Expect format "[PID] name"
                    if (wcsncmp(item, L"[", 1) == 0) {
                        wchar_t *end = nullptr;
                        DWORD listedPid = std::wcstoul(item + 1, &end, 10);
                        if (listedPid == pid) {
                            PostMessageW(combo, CB_SETCURSEL, i, 0);
                            return;
                        }
                    }
                }
            }
        }
    }).detach();
}

void layoutControls(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int padding = 12;
    const int labelWidth = 120;
    const int comboHeight = 24;
    const int editWidth = 120;
    const int buttonWidth = 120;
    const int wideEditWidth = rc.right - labelWidth - buttonWidth - padding * 3;
    const int rowHeight = 28;
    const int checkboxHeight = 20;
    const int statusHeight = comboHeight * 2;

    int x = padding;
    int y = padding;

    SetWindowPos(GetDlgItem(hwnd, kProcessComboId), nullptr, x + labelWidth, y, rc.right - labelWidth - buttonWidth - padding * 3,
                 comboHeight, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, kRefreshButtonId), nullptr, rc.right - buttonWidth - padding, y, buttonWidth, comboHeight,
                 SWP_NOZORDER);

    y += rowHeight;
    SetWindowPos(GetDlgItem(hwnd, kPathEditId), nullptr, x + labelWidth, y, wideEditWidth, comboHeight, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, kLaunchButtonId), nullptr, rc.right - buttonWidth - padding, y, buttonWidth, comboHeight,
                 SWP_NOZORDER);

    y += rowHeight;
    SetWindowPos(GetDlgItem(hwnd, kSpeedEditId), nullptr, x + labelWidth, y, editWidth, comboHeight, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, kIgnoreBgmCheckId), nullptr, x + labelWidth + editWidth + padding, y, 140, checkboxHeight,
                 SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, kApplyButtonId), nullptr, rc.right - buttonWidth - padding, y, buttonWidth, comboHeight,
                 SWP_NOZORDER);

    y += rowHeight;
    SetWindowPos(GetDlgItem(hwnd, kStatusLabelId), nullptr, x, y, rc.right - padding * 2, statusHeight, SWP_NOZORDER);

    if (g_link) {
        int linkHeight = comboHeight + 4;
        SetWindowPos(g_link, nullptr, x, rc.bottom - padding - linkHeight, rc.right - padding * 2, linkHeight, SWP_NOZORDER);
    }
}

HWND createTooltip(HWND parent) {
    HWND tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                   WS_POPUP | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                   parent, nullptr, nullptr, nullptr);
    if (tooltip) {
        SetWindowPos(tooltip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    return tooltip;
}

void addTooltip(HWND tooltip, HWND control, const wchar_t *text) {
    if (!tooltip || !control) return;
    g_state.tooltipTexts.emplace_back(text);

    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = GetParent(control);
    ti.uId = reinterpret_cast<UINT_PTR>(control);
    ti.lpszText = g_state.tooltipTexts.back().data();
    SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        CreateWindowExW(0, L"STATIC", L"Process", WS_CHILD | WS_VISIBLE, 12, 12, 120, 20, hwnd, nullptr, nullptr, nullptr);
        RECT rcClient{};
        GetClientRect(hwnd, &rcClient);
        int initialWidth = rcClient.right - 120 - 120 - 12 * 3;
        HWND combo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", nullptr,
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
                                     132, 10, initialWidth, 200, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProcessComboId)), nullptr, nullptr);
        HWND refresh = CreateWindowExW(0, L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                       0, 10, 100, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshButtonId)), nullptr, nullptr);

        CreateWindowExW(0, L"STATIC", L"Game Path", WS_CHILD | WS_VISIBLE, 12, 40, 120, 20, hwnd, nullptr, nullptr, nullptr);
        HWND pathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                        140, 38, initialWidth, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPathEditId)), nullptr, nullptr);
        HWND launch = CreateWindowExW(0, L"BUTTON", L"Launch + Hook", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      0, 38, 120, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLaunchButtonId)), nullptr, nullptr);

        CreateWindowExW(0, L"STATIC", L"Speed (0.5-2.3)", WS_CHILD | WS_VISIBLE, 12, 68, 120, 20, hwnd, nullptr, nullptr, nullptr);
        HWND speedEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"2.00",
                                         WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                         140, 66, 80, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSpeedEditId)), nullptr, nullptr);
        HWND ignoreBgm = CreateWindowExW(0, L"BUTTON", L"Ignore BGM", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                         140 + 80 + 12, 66, 140, 20, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIgnoreBgmCheckId)), nullptr, nullptr);
        if (ignoreBgm) {
            SendMessageW(ignoreBgm, BM_SETCHECK, g_state.forceAll ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        HWND apply = CreateWindowExW(0, L"BUTTON", L"Hook + Apply", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     0, 66, 120, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kApplyButtonId)), nullptr, nullptr);

        krkrspeed::XAudio2Hook::instance().setUserSpeed(g_state.lastValidSpeed);
        krkrspeed::XAudio2Hook::instance().configureLengthGate(true, g_state.gateSeconds);

        CreateWindowExW(0, L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE,
                        12, 96, 400, 20, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusLabelId)), nullptr, nullptr);

        const wchar_t *linkText = L"<a href=\"https://github.com/caca2331/kirikiri-speed-control\">GitHub: kirikiri-speed-control</a>";
        g_link = CreateWindowExW(0, WC_LINK, linkText,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 12, 124, 500, 24,
                                 hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLinkId)), nullptr, nullptr);
        if (!g_link) {
            // Fallback for systems without SysLink (e.g., missing comctl32 v6 manifest).
            const wchar_t *fallbackText = L"GitHub: https://github.com/caca2331/kirikiri-speed-control";
            g_link = CreateWindowExW(0, L"STATIC", fallbackText,
                                     WS_CHILD | WS_VISIBLE | SS_NOTIFY,
                                     12, 124, 500, 20,
                                     hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLinkId)), nullptr, nullptr);
        }
        if (g_link) {
            SendMessageW(g_link, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        }

        layoutControls(hwnd);
        refreshProcessList(combo, GetDlgItem(hwnd, kStatusLabelId));

        HWND tooltip = createTooltip(hwnd);
        addTooltip(tooltip, combo, L"Select the game process to inject");
        addTooltip(tooltip, refresh, L"Refresh running processes (visible windows in this session)");
        addTooltip(tooltip, pathEdit, L"Full path to game executable; launch suspended, inject, then resume");
        addTooltip(tooltip, launch, L"Launch the game (suspended) and inject matching hook automatically");
        addTooltip(tooltip, speedEdit, L"Target speed (0.5-10.0x, recommended 0.75-2.0x)");
        addTooltip(tooltip, apply, L"Inject DLL and apply speed + gating settings");
        addTooltip(tooltip, ignoreBgm, L"Ignore BGM detection (process all audio at selected speed)");

        if (!g_initialOptions.launchPath.empty()) {
            SetWindowTextW(pathEdit, g_initialOptions.launchPath.c_str());
            PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(kLaunchButtonId, BN_CLICKED),
                         reinterpret_cast<LPARAM>(launch));
        }
        break;
    }
    case WM_SIZE:
        layoutControls(hwnd);
        break;
    case WM_COMMAND: {
        const WORD id = LOWORD(wParam);
        if (id == kRefreshButtonId && HIWORD(wParam) == BN_CLICKED) {
            refreshProcessList(GetDlgItem(hwnd, kProcessComboId), GetDlgItem(hwnd, kStatusLabelId));
        } else if (id == kApplyButtonId && HIWORD(wParam) == BN_CLICKED) {
            handleApply(hwnd);
        } else if (id == kLaunchButtonId && HIWORD(wParam) == BN_CLICKED) {
            handleLaunch(hwnd);
        } else if (id == kLinkId && HIWORD(wParam) == STN_CLICKED) {
            ShellExecuteW(hwnd, L"open", L"https://github.com/caca2331/kirikiri-speed-control", nullptr, nullptr, SW_SHOWNORMAL);
        }
        break;
    }
    case kMsgRefreshQuiet: {
        refreshProcessList(GetDlgItem(hwnd, kProcessComboId), GetDlgItem(hwnd, kStatusLabelId), true);
        return 0;
    }
    case WM_NOTIFY: {
        auto *hdr = reinterpret_cast<NMHDR *>(lParam);
        if (hdr && hdr->idFrom == kLinkId && (hdr->code == NM_CLICK || hdr->code == NM_RETURN)) {
            ShellExecuteW(hwnd, L"open", L"https://github.com/caca2331/kirikiri-speed-control", nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        break;
    }
    case WM_DESTROY:
        KRKR_LOG_INFO("KrkrSpeedController window destroyed, exiting.");
        cleanupSharedMaps();
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

void setInitialOptions(const ControllerOptions &opts) {
    g_initialOptions = opts;
    g_state.enableLog = opts.enableLog;
    g_state.skipDirectSound = opts.skipDirectSound;
    g_state.skipXAudio2 = opts.skipXAudio2;
    g_state.safeMode = opts.safeMode;
    g_state.disableVeh = opts.disableVeh;
    g_state.disableBgm = opts.disableBgm;
    g_state.forceAll = opts.forceAll;
    g_state.bgmSeconds = opts.bgmSeconds;
    g_state.gateSeconds = opts.bgmSeconds;
    g_state.launchPath = opts.launchPath.empty() ? std::filesystem::path{} : std::filesystem::path(opts.launchPath);
    g_state.stereoBgmMode = opts.stereoBgmMode;
}

ControllerOptions getInitialOptions() {
    return g_initialOptions;
}

int runController(HINSTANCE hInstance, int nCmdShow) {
    KRKR_LOG_INFO("KrkrSpeedController GUI starting");
    const wchar_t CLASS_NAME[] = L"KrkrSpeedControllerWindow";

    INITCOMMONCONTROLSEX icc{sizeof(INITCOMMONCONTROLSEX), ICC_WIN95_CLASSES | ICC_LINK_CLASS};
    InitCommonControlsEx(&icc);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, CLASS_NAME, L"Krkr Speed Controller",
                                WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 620, 240,
                                nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        KRKR_LOG_ERROR("Failed to create main window");
        return 0;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

} // namespace krkrspeed::ui
