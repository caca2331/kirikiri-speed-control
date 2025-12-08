#ifndef NOMINMAX
#define NOMINMAX 1
#endif

#include "ui.h"

#include "../common/Logging.h"
#include "../hook/XAudio2Hook.h"

#include <Windows.h>
#include <CommCtrl.h>
#include <TlHelp32.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace krkrspeed::ui {
namespace {

constexpr int kProcessComboId = 1001;
constexpr int kRefreshButtonId = 1002;
constexpr int kSpeedEditId = 1003;
constexpr int kApplyButtonId = 1004;
constexpr int kStatusLabelId = 1005;
constexpr int kDurationEditId = 1006;
constexpr int kDurationCheckboxId = 1007;

struct ProcessInfo {
    std::wstring name;
    DWORD pid = 0;
};

struct AppState {
    std::vector<ProcessInfo> processes;
    float currentSpeed = 1.0f;
    float lastValidSpeed = 1.0f;
    float lastValidDuration = 30.0f;
    bool lengthGateEnabled = true;
    std::vector<std::wstring> tooltipTexts; // keep strings alive for tooltips
};

AppState g_state;

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

float readDurationFromEdit(HWND edit) {
    wchar_t buffer[32] = {};
    GetWindowTextW(edit, buffer, static_cast<int>(std::size(buffer)));
    wchar_t *end = nullptr;
    float parsed = std::wcstof(buffer, &end);
    if (end == buffer || !std::isfinite(parsed)) {
        wchar_t normalized[32] = {};
        swprintf_s(normalized, L"%.2f", g_state.lastValidDuration);
        SetWindowTextW(edit, normalized);
        return g_state.lastValidDuration;
    }
    // Allow reasonable range 0.1s - 600s.
    if (parsed < 0.1f || parsed > 600.0f) {
        wchar_t normalized[32] = {};
        swprintf_s(normalized, L"%.2f", g_state.lastValidDuration);
        SetWindowTextW(edit, normalized);
        return g_state.lastValidDuration;
    }

    g_state.lastValidDuration = parsed;
    return parsed;
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

bool injectDllIntoProcess(DWORD pid, const std::wstring &dllPath, std::wstring &error) {
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                     PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) {
        error = L"OpenProcess failed (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remoteMemory = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMemory) {
        error = L"VirtualAllocEx failed (error " + std::to_wstring(GetLastError()) + L")";
        CloseHandle(process);
        return false;
    }

    if (!WriteProcessMemory(process, remoteMemory, dllPath.c_str(), bytes, nullptr)) {
        error = L"WriteProcessMemory failed (error " + std::to_wstring(GetLastError()) + L")";
        VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel, "LoadLibraryW"));
    if (!loadLibrary) {
        error = L"Unable to resolve LoadLibraryW";
        VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remoteMemory, 0, nullptr);
    if (!thread) {
        error = L"CreateRemoteThread failed (error " + std::to_wstring(GetLastError()) + L")";
        VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    WaitForSingleObject(thread, 5000);
    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);

    CloseHandle(thread);
    VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
    CloseHandle(process);

    if (exitCode == 0) {
        error = L"DLL injection returned 0 (check architecture match)";
        return false;
    }

    KRKR_LOG_INFO("Injected krkr_speed_hook.dll into pid " + std::to_string(pid));
    return true;
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
            outPath = candidate.wstring();
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

void handleApply(HWND hwnd) {
    HWND combo = GetDlgItem(hwnd, kProcessComboId);
    HWND editSpeed = GetDlgItem(hwnd, kSpeedEditId);
    HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);
    HWND editDuration = GetDlgItem(hwnd, kDurationEditId);
    HWND durationCheckbox = GetDlgItem(hwnd, kDurationCheckboxId);

    const float speed = readSpeedFromEdit(editSpeed);
    g_state.currentSpeed = speed;

    const BOOL gateChecked = SendMessageW(durationCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const float duration = readDurationFromEdit(editDuration);
    g_state.lengthGateEnabled = gateChecked != FALSE;

    krkrspeed::XAudio2Hook::instance().setUserSpeed(speed);
    krkrspeed::XAudio2Hook::instance().configureLengthGate(g_state.lengthGateEnabled, duration);

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

    std::wstring dllPath;
    if (!selectHookForArch(targetArch, dllPath, archError)) {
        setStatus(statusLabel, archError);
        return;
    }

    std::wstring error;
    if (injectDllIntoProcess(proc.pid, dllPath, error)) {
        wchar_t message[160] = {};
        swprintf_s(message, L"Injected into %s (PID %u) at %.2fx; gate %s @ %.2fs",
                   proc.name.c_str(), proc.pid, speed, gateChecked ? L"on" : L"off", duration);
        setStatus(statusLabel, message);
    } else {
        setStatus(statusLabel, L"Injection failed: " + error);
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
    const int checkboxWidth = 200;

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
    SetWindowPos(GetDlgItem(hwnd, kDurationEditId), nullptr, x + labelWidth, y, editWidth, comboHeight, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, kDurationCheckboxId), nullptr, x + labelWidth + editWidth + padding, y, checkboxWidth, comboHeight,
                 SWP_NOZORDER);

    y += rowHeight;
    SetWindowPos(GetDlgItem(hwnd, kStatusLabelId), nullptr, x, y, rc.right - padding * 2, comboHeight, SWP_NOZORDER);
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
        HWND speedEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1.00",
                                         WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                         140, 38, 80, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSpeedEditId)), nullptr, nullptr);
        HWND apply = CreateWindowExW(0, L"BUTTON", L"Hook + Apply", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     0, 38, 120, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kApplyButtonId)), nullptr, nullptr);

        CreateWindowExW(0, L"STATIC", L"Max voiced length (seconds)", WS_CHILD | WS_VISIBLE, 12, 68, 180, 20, hwnd, nullptr, nullptr, nullptr);
        HWND durationEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"30.00",
                                            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                            200, 66, 80, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDurationEditId)), nullptr, nullptr);
        HWND durationCheckbox = CreateWindowExW(0, L"BUTTON", L"Only speed-up if shorter than threshold",
                                                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                                0, 66, 240, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDurationCheckboxId)), nullptr, nullptr);
        SendMessageW(durationCheckbox, BM_SETCHECK, BST_CHECKED, 0);
        krkrspeed::XAudio2Hook::instance().setUserSpeed(g_state.lastValidSpeed);
        krkrspeed::XAudio2Hook::instance().configureLengthGate(true, g_state.lastValidDuration);

        CreateWindowExW(0, L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE,
                        12, 96, 400, 20, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusLabelId)), nullptr, nullptr);

        layoutControls(hwnd);
        refreshProcessList(combo, GetDlgItem(hwnd, kStatusLabelId));

        HWND tooltip = createTooltip(hwnd);
        addTooltip(tooltip, combo, L"Select the game process to inject");
        addTooltip(tooltip, refresh, L"Refresh running processes (visible windows in this session)");
        addTooltip(tooltip, speedEdit, L"Target speed (0.5-10.0x, recommended 0.75-2.0x)");
        addTooltip(tooltip, apply, L"Inject DLL and apply speed + gating settings");
        addTooltip(tooltip, durationEdit, L"Buffers shorter than this (seconds) will be time-stretched");
        addTooltip(tooltip, durationCheckbox, L"Enable length-based gating; uncheck to process all audio");
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
        }
        break;
    }
    case WM_DESTROY:
        KRKR_LOG_INFO("KrkrSpeedController window destroyed, exiting.");
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

int runController(HINSTANCE hInstance, int nCmdShow) {
    KRKR_LOG_INFO("KrkrSpeedController GUI starting");
    const wchar_t CLASS_NAME[] = L"KrkrSpeedControllerWindow";

    INITCOMMONCONTROLSEX icc{sizeof(INITCOMMONCONTROLSEX), ICC_WIN95_CLASSES};
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
