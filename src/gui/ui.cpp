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

struct ProcessInfo {
    std::wstring name;
    DWORD pid = 0;
};

struct AppState {
    std::vector<ProcessInfo> processes;
    float currentSpeed = 2.0f;
    float lastValidSpeed = 2.0f;
    float gateSeconds = 60.0f;
    std::vector<std::wstring> tooltipTexts; // keep strings alive for tooltips
    std::map<DWORD, HANDLE> sharedMaps;
    std::map<DWORD, krkrspeed::SharedSettings *> sharedViews;
};

AppState g_state;
static HWND g_link = nullptr;

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
bool injectDllIntoProcess(DWORD pid, const std::wstring &dllPath, std::wstring &error);

bool injectDllIntoProcess(DWORD pid, const std::wstring &dllPath, std::wstring &error) {
    ensureDebugPrivilege();

    // Preflight: ensure we can load the DLL locally (catches missing dependencies).
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

    std::vector<std::wstring> attemptNotes;

    auto tryLoad = [&](const std::wstring &path, size_t attemptIndex, std::wstring &err) -> bool {
        HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                         PROCESS_VM_WRITE | PROCESS_VM_READ,
                                     FALSE, pid);
        if (!process) {
            const DWORD e = GetLastError();
            err = L"OpenProcess failed (error " + std::to_wstring(e) + L")";
            if (e == ERROR_ACCESS_DENIED) {
                err += L"; run the controller as Administrator or ensure the target is not elevated/protected.";
            }
            attemptNotes.push_back(L"#" + std::to_wstring(attemptIndex) + L" " + path + L": " + err);
            return false;
        }

        const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
        LPVOID remoteMemory = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remoteMemory) {
            err = L"VirtualAllocEx failed (error " + std::to_wstring(GetLastError()) + L")";
            CloseHandle(process);
            attemptNotes.push_back(L"#" + std::to_wstring(attemptIndex) + L" " + path + L": " + err);
            return false;
        }

        if (!WriteProcessMemory(process, remoteMemory, path.c_str(), bytes, nullptr)) {
            err = L"WriteProcessMemory failed (error " + std::to_wstring(GetLastError()) + L")";
            VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
            CloseHandle(process);
            attemptNotes.push_back(L"#" + std::to_wstring(attemptIndex) + L" " + path + L": " + err);
            return false;
        }

        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel, "LoadLibraryW"));
        if (!loadLibrary) {
            err = L"Unable to resolve LoadLibraryW";
            VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
            CloseHandle(process);
            attemptNotes.push_back(L"#" + std::to_wstring(attemptIndex) + L" " + path + L": " + err);
            return false;
        }

        HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remoteMemory, 0, nullptr);
        if (!thread) {
            err = L"CreateRemoteThread failed (error " + std::to_wstring(GetLastError()) + L")";
            VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
            CloseHandle(process);
            attemptNotes.push_back(L"#" + std::to_wstring(attemptIndex) + L" " + path + L": " + err);
            return false;
        }

        DWORD waitResult = WaitForSingleObject(thread, 5000);
        DWORD exitCode = 0;
        GetExitCodeThread(thread, &exitCode);

        CloseHandle(thread);
        VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
        CloseHandle(process);

        if (waitResult == WAIT_TIMEOUT) {
            err = L"Injection thread timed out; target may be suspended or blocking remote threads.";
            attemptNotes.push_back(L"#" + std::to_wstring(attemptIndex) + L" " + path + L": " + err);
            return false;
        }
        if (waitResult == WAIT_FAILED) {
            err = L"Injection thread wait failed (error " + std::to_wstring(GetLastError()) + L")";
            attemptNotes.push_back(L"#" + std::to_wstring(attemptIndex) + L" " + path + L": " + err);
            return false;
        }
        if (exitCode != 0) {
            KRKR_LOG_INFO("Injected krkr_speed_hook.dll into pid " + std::to_string(pid) +
                          " using path candidate " + std::to_string(attemptIndex));
            return true;
        }

        err = L"Remote LoadLibraryW returned 0 for path: " + path;
        attemptNotes.push_back(L"#" + std::to_wstring(attemptIndex) + L" " + path + L": " + err);
        return false;
    };

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
        if (tryLoad(p, attemptIdx++, lastError)) {
            return true;
        }
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
                if (tryLoad(p, attemptIdx++, lastError)) {
                    return true;
                }
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

void refreshProcessList(HWND combo, HWND statusLabel) {
    g_state.processes = enumerateProcesses();
    populateProcessCombo(combo, g_state.processes);
    std::wstring status = L"Found " + std::to_wstring(g_state.processes.size()) + L" processes";
    setStatus(statusLabel, status);
}

bool selectHookForArch(ProcessArch arch, std::wstring &outPath, std::wstring &error) {
    const auto baseDir = getExecutableDir();
    if (baseDir.empty()) {
        error = L"Unable to locate controller directory.";
        return false;
    }

    std::vector<std::filesystem::path> candidates;
    if (arch == ProcessArch::X86) {
        candidates = {
            baseDir / L"x86" / L"krkr_speed_hook.dll",
            baseDir / L"krkr_speed_hook32.dll",
            baseDir / L"krkr_speed_hook_x86.dll",
            baseDir / L"krkr_speed_hook.dll" // fallback
        };
    } else {
        candidates = {
            baseDir / L"x64" / L"krkr_speed_hook.dll",
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
    settings.version = 1;
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
    HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);

    const float speed = readSpeedFromEdit(editSpeed);
    g_state.currentSpeed = speed;

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

    constexpr ProcessArch selfArch = getSelfArch();
    if (targetArch != ProcessArch::Unknown && selfArch != ProcessArch::Unknown && targetArch != selfArch) {
        std::wstring msg = L"Architecture mismatch: controller=" + describeArch(selfArch) +
                           L", target=" + describeArch(targetArch) +
                           L". Build/run the " + describeArch(targetArch) +
                           L" version of krkr_speed_hook.dll + KrkrSpeedController.";
        setStatus(statusLabel, msg);
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
    if (injectDllIntoProcess(proc.pid, dllPath, error)) {
        wchar_t message[160] = {};
        swprintf_s(message, L"Injected into %s (PID %u) at %.2fx; gate on @ %.2fs",
                   proc.name.c_str(), proc.pid, speed, g_state.gateSeconds);
        setStatus(statusLabel, message);
    } else {
        std::wstring detail = L" [controller=" + describeArch(selfArch) + L", target=" + describeArch(targetArch) +
                              L", dll=" + describeArch(dllArch) + L"]";
        setStatus(statusLabel, L"Injection failed: " + error + detail);
    }
}

void layoutControls(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int padding = 12;
    const int labelWidth = 200;
    const int comboHeight = 24;
    const int editWidth = 120;
    const int buttonWidth = 120;
    const int rowHeight = 28;

    int x = padding;
    int y = padding;

    SetWindowPos(GetDlgItem(hwnd, kProcessComboId), nullptr, x + labelWidth, y, rc.right - labelWidth - buttonWidth - padding * 3,
                 comboHeight, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, kRefreshButtonId), nullptr, rc.right - buttonWidth - padding, y, buttonWidth, comboHeight,
                 SWP_NOZORDER);

    y += rowHeight;
    SetWindowPos(GetDlgItem(hwnd, kSpeedEditId), nullptr, x + labelWidth, y, editWidth, comboHeight, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, kApplyButtonId), nullptr, rc.right - buttonWidth - padding, y, buttonWidth, comboHeight,
                 SWP_NOZORDER);

    y += rowHeight;
    SetWindowPos(GetDlgItem(hwnd, kStatusLabelId), nullptr, x, y, rc.right - padding * 2, comboHeight, SWP_NOZORDER);
    y += rowHeight;

    if (g_link) {
        SetWindowPos(g_link, nullptr, x, y - 4, rc.right - padding * 2, comboHeight + 4, SWP_NOZORDER);
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
        CreateWindowExW(0, L"STATIC", L"Process", WS_CHILD | WS_VISIBLE, 12, 12, 90, 20, hwnd, nullptr, nullptr, nullptr);
        HWND combo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", nullptr,
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
                                     90, 10, 280, 200, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProcessComboId)), nullptr, nullptr);
        HWND refresh = CreateWindowExW(0, L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                       0, 10, 100, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshButtonId)), nullptr, nullptr);

        CreateWindowExW(0, L"STATIC", L"Speed (0.5-10, suggest 0.75-2)", WS_CHILD | WS_VISIBLE, 12, 40, 220, 20, hwnd, nullptr, nullptr, nullptr);
        HWND speedEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"2.00",
                                         WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                         140, 38, 80, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSpeedEditId)), nullptr, nullptr);
        HWND apply = CreateWindowExW(0, L"BUTTON", L"Hook + Apply", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     0, 38, 120, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kApplyButtonId)), nullptr, nullptr);

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
        addTooltip(tooltip, speedEdit, L"Target speed (0.5-10.0x, recommended 0.75-2.0x)");
        addTooltip(tooltip, apply, L"Inject DLL and apply speed + gating settings");
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
        } else if (id == kLinkId && HIWORD(wParam) == STN_CLICKED) {
            ShellExecuteW(hwnd, L"open", L"https://github.com/caca2331/kirikiri-speed-control", nullptr, nullptr, SW_SHOWNORMAL);
        }
        break;
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
