#include "../common/Logging.h"
#include "../hook/XAudio2Hook.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr int kProcessComboId = 1001;
constexpr int kRefreshButtonId = 1002;
constexpr int kSpeedEditId = 1003;
constexpr int kApplyButtonId = 1004;
constexpr int kStatusLabelId = 1005;

struct ProcessInfo {
    std::wstring name;
    DWORD pid = 0;
};

struct AppState {
    std::vector<ProcessInfo> processes;
    float currentSpeed = 1.0f;
};

AppState g_state;

std::wstring getDllPath() {
    wchar_t buffer[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) == 0) {
        return L"";
    }
    std::filesystem::path path(buffer);
    path = path.parent_path() / L"krkr_speed_hook.dll";
    return path.wstring();
}

std::vector<ProcessInfo> enumerateProcesses() {
    std::vector<ProcessInfo> result;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        KRKR_LOG_ERROR("CreateToolhelp32Snapshot failed: " + std::to_string(GetLastError()));
        return result;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
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
    if (end == buffer) {
        parsed = 1.0f;
    }
    const float clamped = std::clamp(parsed, 0.75f, 2.0f);
    if (clamped != parsed || buffer[0] == L'\0') {
        wchar_t normalized[32] = {};
        swprintf_s(normalized, L"%.2f", clamped);
        SetWindowTextW(edit, normalized);
    }
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

void handleApply(HWND hwnd) {
    HWND combo = GetDlgItem(hwnd, kProcessComboId);
    HWND edit = GetDlgItem(hwnd, kSpeedEditId);
    HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);

    const float speed = readSpeedFromEdit(edit);
    g_state.currentSpeed = speed;
    krkrspeed::XAudio2Hook::instance().setUserSpeed(speed);

    const int index = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(g_state.processes.size())) {
        setStatus(statusLabel, L"Select a process to hook first.");
        return;
    }

    const auto &proc = g_state.processes[static_cast<std::size_t>(index)];
    const std::wstring dllPath = getDllPath();
    if (dllPath.empty()) {
        setStatus(statusLabel, L"Unable to locate krkr_speed_hook.dll next to the controller.");
        return;
    }
    if (!std::filesystem::exists(dllPath)) {
        setStatus(statusLabel, L"krkr_speed_hook.dll was not found beside the controller executable.");
        return;
    }

    std::wstring error;
    if (injectDllIntoProcess(proc.pid, dllPath, error)) {
        wchar_t message[128] = {};
        swprintf_s(message, L"Injected into %s (PID %u) at %.2fx", proc.name.c_str(), proc.pid, speed);
        setStatus(statusLabel, message);
    } else {
        setStatus(statusLabel, L"Injection failed: " + error);
    }
}

void layoutControls(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int padding = 12;
    const int labelWidth = 80;
    const int comboHeight = 24;
    const int editWidth = 100;
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
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        CreateWindowExW(0, L"STATIC", L"Process", WS_CHILD | WS_VISIBLE, 12, 12, 70, 20, hwnd, nullptr, nullptr, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", nullptr,
                        WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
                        90, 10, 280, 200, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProcessComboId)), nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        0, 10, 100, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshButtonId)), nullptr, nullptr);

        CreateWindowExW(0, L"STATIC", L"Speed (0.75-2.0)", WS_CHILD | WS_VISIBLE, 12, 40, 120, 20, hwnd, nullptr, nullptr, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1.00",
                        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                        140, 38, 80, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSpeedEditId)), nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Hook + Apply", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        0, 38, 120, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kApplyButtonId)), nullptr, nullptr);

        CreateWindowExW(0, L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE,
                        12, 68, 400, 20, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusLabelId)), nullptr, nullptr);
        layoutControls(hwnd);
        refreshProcessList(GetDlgItem(hwnd, kProcessComboId), GetDlgItem(hwnd, kStatusLabelId));
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

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    KRKR_LOG_INFO("KrkrSpeedController GUI starting");
    const wchar_t CLASS_NAME[] = L"KrkrSpeedControllerWindow";

    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, CLASS_NAME, L"Krkr Speed Controller",
                                WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 520, 200,
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
