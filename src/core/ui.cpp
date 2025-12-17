#ifndef NOMINMAX
#define NOMINMAX 1
#endif

#include "ui.h"
#include "ControllerCore.h"

#include "../common/Logging.h"
#include <Windows.h>
#include <CommCtrl.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <chrono>
#include <thread>
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
constexpr int kIgnoreBgmLabelId = 1010;
constexpr UINT kMsgRefreshQuiet = WM_APP + 1;

using controller::ProcessArch;
using controller::ProcessInfo;

struct AppState {
    std::vector<ProcessInfo> processes;
    float currentSpeed = 2.0f;
    float lastValidSpeed = 2.0f;
    std::filesystem::path launchPath;
    bool enableLog = false;
    bool skipDirectSound = false;
    bool skipXAudio2 = false;
    bool skipFmod = false;
    bool skipWwise = false;
    bool safeMode = false;
    bool processAllAudio = false;
    float bgmSeconds = 60.0f; // also used as length gate seconds
    std::vector<std::wstring> tooltipTexts; // keep strings alive for tooltips
    std::uint32_t stereoBgmMode = 1;
};

AppState g_state;
static HWND g_link = nullptr;
static ControllerOptions g_initialOptions{};
static HWND g_pathLabel = nullptr;
static HWND g_speedLabel = nullptr;

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

void refreshProcessList(HWND combo, HWND statusLabel, bool quiet = false) {
    g_state.processes = controller::enumerateVisibleProcesses();
    populateProcessCombo(combo, g_state.processes);
    if (!quiet) {
        std::wstring status = L"Found " + std::to_wstring(g_state.processes.size()) + L" processes";
        setStatus(statusLabel, status);
    }
}

bool selectHookForArch(ProcessArch arch, std::wstring &outPath, std::wstring &error) {
    const auto baseDir = controller::controllerDirectory();
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

void handleApply(HWND hwnd) {
    HWND combo = GetDlgItem(hwnd, kProcessComboId);
    HWND editSpeed = GetDlgItem(hwnd, kSpeedEditId);
    HWND ignoreBgm = GetDlgItem(hwnd, kIgnoreBgmCheckId);
    HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);

    const float speed = readSpeedFromEdit(editSpeed);
    g_state.currentSpeed = speed;
    g_state.processAllAudio = (ignoreBgm && SendMessageW(ignoreBgm, BM_GETCHECK, 0, 0) == BST_CHECKED);

    const int index = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(g_state.processes.size())) {
        setStatus(statusLabel, L"Select a process to hook first.");
        return;
    }

    const auto &proc = g_state.processes[static_cast<std::size_t>(index)];

    controller::ProcessArch targetArch = controller::ProcessArch::Unknown;
    std::wstring archError;
    if (!controller::queryProcessArch(proc.pid, targetArch, archError)) {
        setStatus(statusLabel, archError);
        return;
    }

    controller::SharedConfig cfg{};
    cfg.speed = speed;
    cfg.lengthGateEnabled = true;
    cfg.enableLog = g_state.enableLog;
    cfg.skipDirectSound = g_state.skipDirectSound;
    cfg.skipXAudio2 = g_state.skipXAudio2;
    cfg.skipFmod = g_state.skipFmod;
    cfg.skipWwise = g_state.skipWwise;
    cfg.safeMode = g_state.safeMode;
    cfg.processAllAudio = g_state.processAllAudio;
    cfg.stereoBgmMode = g_state.stereoBgmMode;
    cfg.bgmSeconds = g_state.bgmSeconds;

    std::wstring sharedErr;
    if (!controller::writeSharedSettingsForPid(proc.pid, cfg, sharedErr)) {
        setStatus(statusLabel, sharedErr);
        return;
    }

    const auto baseDir = controller::controllerDirectory();
    std::filesystem::path dllPath;
    if (!controller::selectHookForArch(baseDir, targetArch, dllPath, archError)) {
        setStatus(statusLabel, archError);
        return;
    }

    controller::ProcessArch dllArch = controller::ProcessArch::Unknown;
    std::wstring dllArchError;
    if (!controller::getDllArch(dllPath, dllArch, dllArchError)) {
        setStatus(statusLabel, dllArchError);
        return;
    }
    if (targetArch != controller::ProcessArch::Unknown && dllArch != controller::ProcessArch::Unknown && targetArch != dllArch) {
        std::wstring msg = L"Hook DLL arch (" + describeArch(dllArch) + L") does not match target (" +
                           controller::describeArch(targetArch) + L"). Pick the correct dist folder.";
        setStatus(statusLabel, msg);
        return;
    }

    std::wstring error;
    if (controller::injectDllIntoProcess(targetArch, proc.pid, dllPath, error)) {
        wchar_t message[160] = {};
        swprintf_s(message, L"Injected into %s (PID %u) at %.2fx; gate on @ %.2fs",
                   proc.name.c_str(), proc.pid, speed, g_state.bgmSeconds);
        setStatus(statusLabel, message);
    } else {
        const controller::ProcessArch selfArch = controller::getSelfArch();
        std::wstring detail = L" [controller=" + controller::describeArch(selfArch) + L", target=" +
                              controller::describeArch(targetArch) + L", dll=" + controller::describeArch(dllArch) + L"]";
        setStatus(statusLabel, L"Injection failed: " + error + detail);
    }
}

void handleLaunch(HWND hwnd) {
    HWND pathEdit = GetDlgItem(hwnd, kPathEditId);
    HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);
    wchar_t pathBuf[MAX_PATH * 4] = {};
    GetWindowTextW(pathEdit, pathBuf, static_cast<int>(std::size(pathBuf)));
    std::filesystem::path exePath(pathBuf);
    controller::SharedConfig cfg{};
    cfg.speed = g_state.currentSpeed;
    cfg.lengthGateEnabled = true;
    cfg.enableLog = g_state.enableLog;
    cfg.skipDirectSound = g_state.skipDirectSound;
    cfg.skipXAudio2 = g_state.skipXAudio2;
    cfg.skipFmod = g_state.skipFmod;
    cfg.skipWwise = g_state.skipWwise;
    cfg.safeMode = g_state.safeMode;
    cfg.processAllAudio = g_state.processAllAudio;
    cfg.stereoBgmMode = g_state.stereoBgmMode;
    cfg.bgmSeconds = g_state.bgmSeconds;

    DWORD launchedPid = 0;
    std::wstring err;
    if (controller::launchAndInject(exePath, cfg, launchedPid, err)) {
        setStatus(statusLabel, L"Launched and injected: " + exePath.filename().wstring() +
                   L" (PID " + std::to_wstring(launchedPid) + L")");
    } else {
        setStatus(statusLabel, err);
        return;
    }
    // Refresh process list and select the new one (best-effort, 3s timeout, non-blocking UI thread).
    std::thread([hwnd, pid = launchedPid]() {
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

    // TEMP: hide launch-by-path row (keep controls created for easy restore). Move offscreen.
    if (g_pathLabel) {
        SetWindowPos(g_pathLabel, nullptr, -5000, -5000, 120, 20, SWP_NOZORDER);
    }
    SetWindowPos(GetDlgItem(hwnd, kPathEditId), nullptr, -5000, -5000, wideEditWidth, comboHeight, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, kLaunchButtonId), nullptr, -5000, -5000, buttonWidth, comboHeight, SWP_NOZORDER);
    // To re-enable: restore SetWindowPos to original positions and keep y += rowHeight;

    y += rowHeight; // keep vertical rhythm for speed row even while launch row is hidden
    if (g_speedLabel) {
        SetWindowPos(g_speedLabel, nullptr, x, y + 2, labelWidth, comboHeight, SWP_NOZORDER);
    }
    SetWindowPos(GetDlgItem(hwnd, kSpeedEditId), nullptr, x + labelWidth, y, editWidth, comboHeight, SWP_NOZORDER);
    // Place ignore-BGM label and checkbox together
    SetWindowPos(GetDlgItem(hwnd, kIgnoreBgmLabelId), nullptr, x + labelWidth + editWidth + padding, y + 2, 90, checkboxHeight,
                 SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, kIgnoreBgmCheckId), nullptr, x + labelWidth + editWidth + padding + 94, y, 20, checkboxHeight,
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

        g_pathLabel = CreateWindowExW(0, L"STATIC", L"Game Path", WS_CHILD | WS_VISIBLE, 12, 40, 120, 20, hwnd, nullptr, nullptr, nullptr);
        HWND pathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                        140, 38, initialWidth, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPathEditId)), nullptr, nullptr);
        HWND launch = CreateWindowExW(0, L"BUTTON", L"Launch + Hook", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      0, 38, 120, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLaunchButtonId)), nullptr, nullptr);

        g_speedLabel = CreateWindowExW(0, L"STATIC", L"Speed (0.5-2.3)", WS_CHILD | WS_VISIBLE, 12, 68, 120, 20, hwnd, nullptr, nullptr, nullptr);
        HWND speedEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"2.00",
                                         WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                         140, 66, 80, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSpeedEditId)), nullptr, nullptr);
        HWND ignoreBgm = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                         140 + 80 + 12 + 90, 66, 20, 20, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIgnoreBgmCheckId)), nullptr, nullptr);
        HWND ignoreBgmLabel = CreateWindowExW(0, L"STATIC", L"Process BGM", WS_CHILD | WS_VISIBLE,
                                              140 + 80 + 12, 66, 90, 20, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIgnoreBgmLabelId)), nullptr, nullptr);
        if (ignoreBgm) {
            SendMessageW(ignoreBgm, BM_SETCHECK, g_state.processAllAudio ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        HWND apply = CreateWindowExW(0, L"BUTTON", L"Hook + Apply", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     0, 66, 120, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kApplyButtonId)), nullptr, nullptr);

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
        // Tooltip removed for Process BGM for now.

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
    g_state.skipFmod = opts.skipFmod;
    g_state.skipWwise = opts.skipWwise;
    g_state.safeMode = opts.safeMode;
    g_state.processAllAudio = opts.processAllAudio;
    g_state.bgmSeconds = opts.bgmSeconds;
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
