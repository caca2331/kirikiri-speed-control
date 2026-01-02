#ifndef NOMINMAX
#define NOMINMAX 1
#endif

#include "ui.h"
#include "ControllerCore.h"

#include "../common/Logging.h"
#include "../common/UiText.h"
#include <Windows.h>
#include <CommCtrl.h>
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <chrono>
#include <thread>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <limits>

namespace krkrspeed::ui {
namespace {

constexpr int kProcessComboId = 1001;
constexpr int kRefreshButtonId = 1002;
constexpr int kSpeedEditId = 1003;
constexpr int kStatusLabelId = 1005;
constexpr int kLinkId = 1006;
constexpr int kIgnoreBgmCheckId = 1009;
constexpr int kIgnoreBgmLabelId = 1010;
constexpr int kAutoHookCheckId = 1011;
constexpr int kAutoHookLabelId = 1012;
constexpr int kLanguageComboId = 1013;
constexpr int kAutoHookDelayCheckId = 1014;
constexpr int kAutoHookDelayLabelId = 1015;
constexpr UINT kAutoHookTimerId = 3001;
constexpr UINT kAutoHookIntervalMs = 2000;
constexpr UINT kMsgAutoSelectPid = WM_APP + 2;
constexpr UINT kMsgRefreshQuiet = WM_APP + 1;
constexpr int kHotkeyToggleSpeedId = 2001;
constexpr int kHotkeySpeedUpId = 2002;
constexpr int kHotkeySpeedDownId = 2003;
constexpr UINT kHotkeyModifiers = MOD_ALT;

using controller::ProcessArch;
using controller::ProcessInfo;
using controller::SpeedControlState;
using controller::SpeedHotkeyAction;
using ui_text::UiTextId;

bool getSelectedProcess(HWND hwnd, ProcessInfo &proc, std::wstring &error);
void refreshProcessList(HWND combo, HWND statusLabel, bool quiet);
controller::SharedConfig buildSharedConfig(float speed);
void updateAutoHookCheckbox(HWND hwnd);
void updateProcessBgmCheckbox(HWND hwnd);
void updateAutoHookDelayControls(HWND hwnd);
void updateHookButtonState(HWND hwnd);
bool pruneHookedPids(const std::unordered_set<DWORD> &current);
void ensureUiTextLoaded();
void refreshUiText(HWND hwnd);
void updateTooltips();
void updateTrackedTooltip(const MSG &msg);
TOOLINFOW makeToolInfo(HWND control, UiTextId textId);
int getTextHeight(HWND hwnd, int fallback);
HFONT createHotkeyFont(HWND reference);
void handleAutoHookDelayToggle(HWND hwnd);

std::wstring toLowerCopy(const std::wstring &input) {
    std::wstring out;
    out.reserve(input.size());
    for (wchar_t c : input) {
        out.push_back(static_cast<wchar_t>(std::towlower(c)));
    }
    return out;
}

void refreshProcessUi(HWND hwnd, HWND combo, HWND statusLabel) {
    if (!combo || !statusLabel) return;
    refreshProcessList(combo, statusLabel, true);
    updateAutoHookCheckbox(hwnd);
    updateProcessBgmCheckbox(hwnd);
    const auto sessionProcesses = controller::enumerateSessionProcesses();
    std::unordered_set<DWORD> current;
    current.reserve(sessionProcesses.size());
    for (const auto &proc : sessionProcesses) {
        current.insert(proc.pid);
    }
    pruneHookedPids(current);
    updateHookButtonState(hwnd);
}

struct AppState {
    std::vector<ProcessInfo> processes;
    SpeedControlState speed;
    std::filesystem::path launchPath;
    bool enableLog = false;
    bool skipDirectSound = false;
    bool skipXAudio2 = false;
    bool skipFmod = false;
    bool skipWwise = false;
    bool safeMode = false;
    bool processAllAudio = false;
    float bgmSeconds = 60.0f; // also used as length gate seconds
    std::uint32_t stereoBgmMode = 1;
    std::wstring searchTerm;
};

AppState g_state;
static HWND g_link = nullptr;
static bool g_linkIsSysLink = false;
static ControllerOptions g_initialOptions{};
static HWND g_processLabel = nullptr;
static HWND g_speedLabel = nullptr;
static HWND g_autoHookCheck = nullptr;
static HWND g_autoHookLabel = nullptr;
static HWND g_autoHookDelayCheck = nullptr;
static HWND g_autoHookDelayLabel = nullptr;
static HWND g_ignoreBgmLabel = nullptr;
static HWND g_languageCombo = nullptr;
static HWND g_hotkeyLabel = nullptr;
static HWND g_tooltip = nullptr;
static std::unordered_map<std::uintptr_t, UiTextId> g_tooltipById;
static HWND g_mainWindow = nullptr;
static HWND g_activeTooltipControl = nullptr;
static HFONT g_hotkeyFont = nullptr;
static std::unordered_set<DWORD> g_knownPids;
static std::unordered_set<DWORD> g_autoHookAttempted;
static std::unordered_set<DWORD> g_hookedPids;
static DWORD g_pendingAutoSelectPid = 0;
static bool g_pendingAutoHookRefresh = false;
static WNDPROC g_speedEditProc = nullptr;

void setStatus(HWND statusLabel, const std::wstring &text) {
    SetWindowTextW(statusLabel, text.c_str());
    KRKR_LOG_INFO(text);
}

void ensureUiTextLoaded() {
    static bool loaded = false;
    if (loaded) return;
    std::wstring error;
    auto path = controller::controllerDirectory() / L"ui_texts.yaml";
    if (!ui_text::LoadUiTextPacks(path, error)) {
        if (!error.empty()) {
            KRKR_LOG_WARN(L"UiText load failed: " + error);
        }
    }
    ui_text::SetUiLanguage(L"zh-CN");
    loaded = true;
}

void refreshUiText(HWND hwnd) {
    if (!hwnd) return;
    if (g_processLabel) {
        SetWindowTextW(g_processLabel, ui_text::UiText(UiTextId::LabelProcess).c_str());
    }
    if (g_speedLabel) {
        SetWindowTextW(g_speedLabel, ui_text::UiText(UiTextId::LabelSpeed).c_str());
    }
    if (g_ignoreBgmLabel) {
        SetWindowTextW(g_ignoreBgmLabel, ui_text::UiText(UiTextId::LabelProcessBgm).c_str());
    }
    if (g_autoHookLabel) {
        SetWindowTextW(g_autoHookLabel, ui_text::UiText(UiTextId::LabelAutoHook).c_str());
    }
    if (g_autoHookDelayLabel) {
        SetWindowTextW(g_autoHookDelayLabel, ui_text::UiText(UiTextId::LabelAutoHookDelay).c_str());
    }
    if (g_hotkeyLabel) {
        SetWindowTextW(g_hotkeyLabel, ui_text::UiText(UiTextId::LabelHotkey).c_str());
    }
    if (g_link) {
        const auto &linkText = g_linkIsSysLink ? ui_text::UiText(UiTextId::LinkMarkup)
                                               : ui_text::UiText(UiTextId::LinkPlain);
        SetWindowTextW(g_link, linkText.c_str());
    }
    SetWindowTextW(hwnd, ui_text::UiText(UiTextId::WindowTitle).c_str());

    updateHookButtonState(hwnd);
    updateTooltips();

}

float readSpeedFromEdit(HWND edit) {
    wchar_t buffer[32] = {};
    GetWindowTextW(edit, buffer, static_cast<int>(std::size(buffer)));
    wchar_t *end = nullptr;
    float parsed = std::wcstof(buffer, &end);
    if (end == buffer || !std::isfinite(parsed)) {
        // Restore previous valid value.
        wchar_t normalized[32] = {};
        swprintf_s(normalized, L"%.2f", g_state.speed.lastValidSpeed);
        SetWindowTextW(edit, normalized);
        controller::updateSpeedFromInput(g_state.speed, g_state.speed.lastValidSpeed);
        return g_state.speed.lastValidSpeed;
    }
    if (parsed < 0.5f || parsed > 10.0f) {
        wchar_t normalized[32] = {};
        swprintf_s(normalized, L"%.2f", g_state.speed.lastValidSpeed);
        SetWindowTextW(edit, normalized);
        controller::updateSpeedFromInput(g_state.speed, g_state.speed.lastValidSpeed);
        return g_state.speed.lastValidSpeed;
    }

    controller::updateSpeedFromInput(g_state.speed, parsed);
    return g_state.speed.currentSpeed;
}

void writeSpeedEdit(HWND hwnd) {
    HWND edit = GetDlgItem(hwnd, kSpeedEditId);
    if (!edit) return;
    wchar_t normalized[32] = {};
    swprintf_s(normalized, L"%.2f", g_state.speed.currentSpeed);
    SetWindowTextW(edit, normalized);
}

void syncProcessAllAudioFromCheckbox(HWND hwnd) {
    HWND ignoreBgm = GetDlgItem(hwnd, kIgnoreBgmCheckId);
    if (ignoreBgm) {
        g_state.processAllAudio = (SendMessageW(ignoreBgm, BM_GETCHECK, 0, 0) == BST_CHECKED);
    }
}

void updateAutoHookCheckbox(HWND hwnd) {
    if (!g_autoHookCheck) return;
    ProcessInfo proc;
    std::wstring error;
    if (!getSelectedProcess(hwnd, proc, error)) {
        SendMessageW(g_autoHookCheck, BM_SETCHECK, BST_UNCHECKED, 0);
        updateAutoHookDelayControls(hwnd);
        return;
    }
    std::wstring exePath;
    if (!controller::getProcessExePath(proc.pid, exePath, error)) {
        SendMessageW(g_autoHookCheck, BM_SETCHECK, BST_UNCHECKED, 0);
        updateAutoHookDelayControls(hwnd);
        return;
    }
    const bool enabled = controller::isAutoHookEnabled(exePath, proc.name);
    SendMessageW(g_autoHookCheck, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    updateAutoHookDelayControls(hwnd);
}

void updateProcessBgmCheckbox(HWND hwnd) {
    HWND ignoreBgm = GetDlgItem(hwnd, kIgnoreBgmCheckId);
    if (!ignoreBgm) return;
    ProcessInfo proc;
    std::wstring error;
    if (!getSelectedProcess(hwnd, proc, error)) {
        SendMessageW(ignoreBgm, BM_SETCHECK, BST_UNCHECKED, 0);
        g_state.processAllAudio = false;
        return;
    }
    std::wstring exePath;
    if (!controller::getProcessExePath(proc.pid, exePath, error)) {
        SendMessageW(ignoreBgm, BM_SETCHECK, BST_UNCHECKED, 0);
        g_state.processAllAudio = false;
        return;
    }
    const bool enabled = controller::isProcessBgmEnabled(exePath, proc.name);
    SendMessageW(ignoreBgm, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    g_state.processAllAudio = enabled;
}

void updateAutoHookDelayControls(HWND hwnd) {
    if (!g_autoHookDelayLabel || !g_autoHookDelayCheck) return;
    const bool autoHookEnabled =
        g_autoHookCheck && (SendMessageW(g_autoHookCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
    ShowWindow(g_autoHookDelayLabel, autoHookEnabled ? SW_SHOW : SW_HIDE);
    ShowWindow(g_autoHookDelayCheck, autoHookEnabled ? SW_SHOW : SW_HIDE);
    if (!autoHookEnabled) {
        SendMessageW(g_autoHookDelayCheck, BM_SETCHECK, BST_UNCHECKED, 0);
        return;
    }
    ProcessInfo proc;
    std::wstring error;
    if (!getSelectedProcess(hwnd, proc, error)) {
        SendMessageW(g_autoHookDelayCheck, BM_SETCHECK, BST_UNCHECKED, 0);
        return;
    }
    std::wstring exePath;
    if (!controller::getProcessExePath(proc.pid, exePath, error)) {
        SendMessageW(g_autoHookDelayCheck, BM_SETCHECK, BST_UNCHECKED, 0);
        return;
    }
    double delaySeconds = 0.0;
    const bool hasDelay = controller::tryGetAutoHookDelay(exePath, proc.name, delaySeconds);
    SendMessageW(g_autoHookDelayCheck, BM_SETCHECK, hasDelay ? BST_CHECKED : BST_UNCHECKED, 0);
}

void updateHookButtonState(HWND hwnd) {
    HWND hookButton = GetDlgItem(hwnd, kRefreshButtonId);
    if (!hookButton) return;

    ProcessInfo proc;
    std::wstring error;
    if (!getSelectedProcess(hwnd, proc, error)) {
        SetWindowTextW(hookButton, L"Hook");
        EnableWindow(hookButton, TRUE);
        return;
    }

    const bool hooked = (g_hookedPids.find(proc.pid) != g_hookedPids.end());
    if (hooked) {
        SetWindowTextW(hookButton, ui_text::UiText(UiTextId::ButtonHooked).c_str());
        EnableWindow(hookButton, FALSE);
    } else {
        SetWindowTextW(hookButton, ui_text::UiText(UiTextId::ButtonHook).c_str());
        EnableWindow(hookButton, TRUE);
    }
}

void applySettingsToSelectedIfHooked(HWND hwnd) {
    ProcessInfo proc;
    std::wstring error;
    if (!getSelectedProcess(hwnd, proc, error)) {
        return;
    }
    if (g_hookedPids.find(proc.pid) == g_hookedPids.end()) {
        return;
    }
    controller::SharedConfig cfg = buildSharedConfig(g_state.speed.currentSpeed);
    if (!controller::applySpeedToPid(proc.pid, cfg, g_state.speed, error)) {
        setStatus(GetDlgItem(hwnd, kStatusLabelId), error);
    }
}

bool selectProcessByPid(HWND hwnd, DWORD pid) {
    HWND combo = GetDlgItem(hwnd, kProcessComboId);
    if (!combo || pid == 0) return false;
    const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    wchar_t item[256] = {};
    for (int i = 0; i < count; ++i) {
        if (SendMessageW(combo, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(item)) > 0) {
            if (wcsncmp(item, L"[", 1) == 0) {
                wchar_t *end = nullptr;
                DWORD listedPid = std::wcstoul(item + 1, &end, 10);
                if (listedPid == pid) {
                    SendMessageW(combo, CB_SETCURSEL, i, 0);
                    updateAutoHookCheckbox(hwnd);
                    updateProcessBgmCheckbox(hwnd);
                    updateHookButtonState(hwnd);
                    return true;
                }
            }
        }
    }
    return false;
}

LRESULT CALLBACK SpeedEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_ESCAPE) {
            wchar_t normalized[32] = {};
            swprintf_s(normalized, L"%.2f", g_state.speed.lastValidSpeed);
            SetWindowTextW(hwnd, normalized);
            SetFocus(GetParent(hwnd));
            return 0;
        }
        if (wParam == VK_RETURN) {
            SetFocus(GetParent(hwnd));
            return 0;
        }
    } else if (msg == WM_CHAR) {
        if (wParam == VK_ESCAPE || wParam == VK_RETURN) {
            return 0;
        }
    }
    return CallWindowProcW(g_speedEditProc, hwnd, msg, wParam, lParam);
}

void initKnownPids() {
    g_knownPids.clear();
    g_autoHookAttempted.clear();
    const auto sessionProcesses = controller::enumerateSessionProcesses();
    for (const auto &proc : sessionProcesses) {
        g_knownPids.insert(proc.pid);
    }
}

bool pruneHookedPids(const std::unordered_set<DWORD> &current) {
    bool removed = false;
    for (auto it = g_hookedPids.begin(); it != g_hookedPids.end();) {
        if (current.find(*it) == current.end()) {
            it = g_hookedPids.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    return removed;
}

void scheduleAutoHook(HWND hwnd, const ProcessInfo &proc, controller::SharedConfig cfg, double delaySeconds) {
    std::thread([hwnd, proc, cfg, delaySeconds]() {
        if (delaySeconds > 0.0) {
            KRKR_LOG_INFO("Auto-hook delay " + std::to_string(delaySeconds) +
                          "s for pid=" + std::to_string(proc.pid));
            std::this_thread::sleep_for(std::chrono::duration<double>(delaySeconds));
        }
        std::wstring err;
        controller::ProcessArch targetArch = controller::ProcessArch::Unknown;
        if (!controller::queryProcessArch(proc.pid, targetArch, err)) {
            KRKR_LOG_WARN(std::string("Auto-hook arch query failed for pid=") + std::to_string(proc.pid));
            return;
        }
        if (!controller::writeSharedSettingsForPid(proc.pid, cfg, err)) {
            KRKR_LOG_WARN(std::string("Auto-hook shared settings failed for pid=") + std::to_string(proc.pid));
            return;
        }

        const auto baseDir = controller::controllerDirectory();
        std::filesystem::path dllPath;
        std::wstring archError;
        if (!controller::selectHookForArch(baseDir, targetArch, dllPath, archError)) {
            KRKR_LOG_WARN("Auto-hook selectHookForArch failed");
            return;
        }

        controller::ProcessArch dllArch = controller::ProcessArch::Unknown;
        std::wstring dllArchError;
        if (!controller::getDllArch(dllPath, dllArch, dllArchError)) {
            KRKR_LOG_WARN("Auto-hook getDllArch failed");
            return;
        }
        if (targetArch != controller::ProcessArch::Unknown && dllArch != controller::ProcessArch::Unknown &&
            targetArch != dllArch) {
            KRKR_LOG_WARN("Auto-hook DLL arch mismatch");
            return;
        }

        if (!controller::injectDllIntoProcess(targetArch, proc.pid, dllPath, err)) {
            KRKR_LOG_WARN(std::string("Auto-hook inject failed for pid=") + std::to_string(proc.pid));
            return;
        }
        KRKR_LOG_INFO(std::string("Auto-hook injected pid=") + std::to_string(proc.pid));
        if (hwnd) {
            PostMessageW(hwnd, kMsgAutoSelectPid, static_cast<WPARAM>(proc.pid), 0);
        }
    }).detach();
}

void scheduleAutoHooksForProcesses(HWND hwnd, const std::vector<ProcessInfo> &processes, bool updateUi) {
    if (processes.empty() || controller::autoHookEntryCount() == 0) {
        return;
    }

    HWND editSpeed = GetDlgItem(hwnd, kSpeedEditId);
    if (editSpeed) {
        readSpeedFromEdit(editSpeed);
    }
    syncProcessAllAudioFromCheckbox(hwnd);
    const float effectiveSpeed = controller::effectiveSpeed(g_state.speed);
    controller::SharedConfig cfg = buildSharedConfig(effectiveSpeed);

    std::unordered_set<std::wstring> autoHookNames;
    {
        const auto names = controller::autoHookExeNames();
        autoHookNames.reserve(names.size());
        for (const auto &name : names) {
            autoHookNames.insert(toLowerCopy(name));
        }
    }

    HWND combo = nullptr;
    HWND statusLabel = nullptr;
    bool refreshedUi = false;
    if (updateUi) {
        combo = GetDlgItem(hwnd, kProcessComboId);
        statusLabel = GetDlgItem(hwnd, kStatusLabelId);
    }

    for (const auto &proc : processes) {
        if (g_autoHookAttempted.find(proc.pid) != g_autoHookAttempted.end()) continue;
        if (!autoHookNames.empty()) {
            const std::wstring nameLower = toLowerCopy(proc.name);
            if (autoHookNames.find(nameLower) == autoHookNames.end()) {
                continue;
            }
        }
        std::wstring exePath;
        std::wstring error;
        if (!controller::getProcessExePath(proc.pid, exePath, error)) {
            continue;
        }
        if (!controller::isAutoHookEnabled(exePath, proc.name)) {
            continue;
        }
        controller::SharedConfig procCfg = cfg;
        procCfg.processAllAudio = controller::isProcessBgmEnabled(exePath, proc.name);
        double delaySeconds = 0.0;
        controller::tryGetAutoHookDelay(exePath, proc.name, delaySeconds);
        g_autoHookAttempted.insert(proc.pid);

        if (updateUi && combo && statusLabel) {
            if (!refreshedUi) {
                refreshProcessUi(hwnd, combo, statusLabel);
                refreshedUi = true;
            }
            if (!selectProcessByPid(hwnd, proc.pid)) {
                g_pendingAutoSelectPid = proc.pid;
                g_pendingAutoHookRefresh = true;
            }
        }

        scheduleAutoHook(hwnd, proc, procCfg, delaySeconds);
    }
}

void pollAutoHook(HWND hwnd) {
    HWND combo = GetDlgItem(hwnd, kProcessComboId);
    HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);
    if (!combo || !statusLabel) return;

    if (g_pendingAutoHookRefresh) {
        refreshProcessUi(hwnd, combo, statusLabel);
        bool selected = false;
        if (g_pendingAutoSelectPid != 0) {
            selected = selectProcessByPid(hwnd, g_pendingAutoSelectPid);
            if (selected) {
                g_pendingAutoSelectPid = 0;
            }
        }
        if (selected || g_pendingAutoSelectPid == 0) {
            g_pendingAutoHookRefresh = false;
        }
    } else if (g_pendingAutoSelectPid != 0) {
        if (selectProcessByPid(hwnd, g_pendingAutoSelectPid)) {
            g_pendingAutoSelectPid = 0;
        }
    }

    if (controller::autoHookEntryCount() == 0) {
        return;
    }

    std::unordered_set<DWORD> current;
    std::vector<ProcessInfo> newProcesses;
    const auto sessionProcesses = controller::enumerateSessionProcesses();
    current.reserve(sessionProcesses.size());
    for (const auto &proc : sessionProcesses) {
        current.insert(proc.pid);
        if (g_knownPids.find(proc.pid) == g_knownPids.end()) {
            newProcesses.push_back(proc);
        }
    }

    bool removedInjected = false;
    for (auto it = g_autoHookAttempted.begin(); it != g_autoHookAttempted.end();) {
        if (current.find(*it) == current.end()) {
            it = g_autoHookAttempted.erase(it);
            removedInjected = true;
        } else {
            ++it;
        }
    }

    const bool removedHooked = pruneHookedPids(current);
    bool refreshed = false;
    if (removedHooked) {
        refreshProcessUi(hwnd, combo, statusLabel);
        refreshed = true;
    }
    if (removedInjected && !refreshed) {
        refreshProcessUi(hwnd, combo, statusLabel);
    }

    if (g_pendingAutoSelectPid != 0 && current.find(g_pendingAutoSelectPid) == current.end()) {
        g_pendingAutoSelectPid = 0;
        g_pendingAutoHookRefresh = false;
    }

    g_knownPids = std::move(current);

    if (newProcesses.empty()) return;
    scheduleAutoHooksForProcesses(hwnd, newProcesses, true);
}

void handleAutoHookToggle(HWND hwnd) {
    if (!g_autoHookCheck) return;
    HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);
    ProcessInfo proc;
    std::wstring error;
    if (!getSelectedProcess(hwnd, proc, error)) {
        setStatus(statusLabel, error);
        return;
    }
    std::wstring exePath;
    if (!controller::getProcessExePath(proc.pid, exePath, error)) {
        setStatus(statusLabel, error);
        return;
    }
    const bool checked = (SendMessageW(g_autoHookCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (!controller::setAutoHookEnabled(exePath, proc.name, checked, error)) {
        setStatus(statusLabel, error);
        return;
    }
    std::wstring msg = checked ? (L"Auto-hook enabled for " + proc.name)
                               : (L"Auto-hook disabled for " + proc.name);
    setStatus(statusLabel, msg);
    if (controller::autoHookEntryCount() > 0) {
        initKnownPids();
    }
    updateAutoHookDelayControls(hwnd);
}

void handleAutoHookDelayToggle(HWND hwnd) {
    if (!g_autoHookDelayCheck) return;
    HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);
    ProcessInfo proc;
    std::wstring error;
    if (!getSelectedProcess(hwnd, proc, error)) {
        setStatus(statusLabel, error);
        return;
    }
    std::wstring exePath;
    if (!controller::getProcessExePath(proc.pid, exePath, error)) {
        setStatus(statusLabel, error);
        return;
    }
    const bool checked = (SendMessageW(g_autoHookDelayCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
    const double delaySeconds = checked ? 15.0 : 0.0;
    if (!controller::setAutoHookDelay(exePath, proc.name, checked, delaySeconds, error)) {
        setStatus(statusLabel, error);
        return;
    }
    std::wstring msg = checked ? (L"Auto-hook delay enabled for " + proc.name)
                               : (L"Auto-hook delay disabled for " + proc.name);
    setStatus(statusLabel, msg);
}

void handleProcessBgmToggle(HWND hwnd) {
    HWND ignoreBgm = GetDlgItem(hwnd, kIgnoreBgmCheckId);
    if (!ignoreBgm) return;
    HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);
    ProcessInfo proc;
    std::wstring error;
    if (!getSelectedProcess(hwnd, proc, error)) {
        setStatus(statusLabel, error);
        return;
    }
    std::wstring exePath;
    if (!controller::getProcessExePath(proc.pid, exePath, error)) {
        setStatus(statusLabel, error);
        return;
    }
    const bool checked = (SendMessageW(ignoreBgm, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (!controller::setProcessBgmEnabled(exePath, proc.name, checked, error)) {
        setStatus(statusLabel, error);
        return;
    }
    std::wstring msg = checked ? (L"Process BGM enabled for " + proc.name)
                               : (L"Process BGM disabled for " + proc.name);
    setStatus(statusLabel, msg);
}

controller::SharedConfig buildSharedConfig(float speed) {
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
    return cfg;
}

bool getSelectedProcess(HWND hwnd, ProcessInfo &proc, std::wstring &error) {
    HWND combo = GetDlgItem(hwnd, kProcessComboId);
    const int index = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(g_state.processes.size())) {
        error = L"Select a process first.";
        return false;
    }
    proc = g_state.processes[static_cast<std::size_t>(index)];
    return true;
}

void populateProcessCombo(HWND combo, const std::vector<ProcessInfo> &processes) {
    int prevSel = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    DWORD prevPid = 0;
    if (prevSel >= 0) {
        wchar_t item[256] = {};
        if (SendMessageW(combo, CB_GETLBTEXT, prevSel, reinterpret_cast<LPARAM>(item)) > 0) {
            if (wcsncmp(item, L"[", 1) == 0) {
                wchar_t *end = nullptr;
                prevPid = std::wcstoul(item + 1, &end, 10);
            }
        }
    }

    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int restoredIndex = -1;
    for (size_t i = 0; i < processes.size(); ++i) {
        const auto &proc = processes[i];
        std::wstring label = L"[" + std::to_wstring(proc.pid) + L"] " + proc.name;
        if (!proc.windowTitle.empty()) {
            label += L" | " + proc.windowTitle;
        }
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (prevPid != 0 && proc.pid == prevPid) {
            restoredIndex = static_cast<int>(i);
        }
    }
    if (!processes.empty()) {
        SendMessageW(combo, CB_SETCURSEL, restoredIndex >= 0 ? restoredIndex : 0, 0);
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

void handleApply(HWND hwnd) {
    HWND editSpeed = GetDlgItem(hwnd, kSpeedEditId);
    HWND ignoreBgm = GetDlgItem(hwnd, kIgnoreBgmCheckId);
    HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);

    readSpeedFromEdit(editSpeed);
    g_state.processAllAudio = (ignoreBgm && SendMessageW(ignoreBgm, BM_GETCHECK, 0, 0) == BST_CHECKED);

    ProcessInfo proc;
    std::wstring error;
    if (!getSelectedProcess(hwnd, proc, error)) {
        setStatus(statusLabel, error);
        return;
    }

    controller::ProcessArch targetArch = controller::ProcessArch::Unknown;
    std::wstring archError;
    if (!controller::queryProcessArch(proc.pid, targetArch, archError)) {
        setStatus(statusLabel, archError);
        return;
    }

    controller::SharedConfig cfg = buildSharedConfig(g_state.speed.currentSpeed);
    std::wstring sharedErr;
    if (!controller::applySpeedToPid(proc.pid, cfg, g_state.speed, sharedErr)) {
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

    if (controller::injectDllIntoProcess(targetArch, proc.pid, dllPath, error)) {
        g_hookedPids.insert(proc.pid);
        updateHookButtonState(hwnd);
        wchar_t message[160] = {};
        const float effectiveSpeed = controller::effectiveSpeed(g_state.speed);
        if (g_state.speed.enabled) {
            swprintf_s(message, L"Injected into %s (PID %u) at %.2fx; gate on @ %.2fs",
                       proc.name.c_str(), proc.pid, effectiveSpeed, g_state.bgmSeconds);
        } else {
            swprintf_s(message, L"Injected into %s (PID %u) at %.2fx (speed off); gate on @ %.2fs",
                       proc.name.c_str(), proc.pid, effectiveSpeed, g_state.bgmSeconds);
        }
        setStatus(statusLabel, message);
    } else {
        const controller::ProcessArch selfArch = controller::getSelfArch();
        std::wstring detail = L" [controller=" + controller::describeArch(selfArch) + L", target=" +
                              controller::describeArch(targetArch) + L", dll=" + controller::describeArch(dllArch) + L"]";
        setStatus(statusLabel, L"Injection failed: " + error + detail);
    }
}

void handleLaunch(HWND hwnd) {
    HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);
    HWND editSpeed = GetDlgItem(hwnd, kSpeedEditId);
    if (editSpeed) {
        readSpeedFromEdit(editSpeed);
    }
    if (g_state.launchPath.empty()) {
        setStatus(statusLabel, L"Launch path not set.");
        return;
    }
    const std::filesystem::path exePath = g_state.launchPath;
    const float effectiveSpeed = controller::effectiveSpeed(g_state.speed);
    controller::SharedConfig cfg = buildSharedConfig(effectiveSpeed);

    DWORD launchedPid = 0;
    std::wstring err;
    if (controller::launchAndInject(exePath, cfg, launchedPid, err)) {
        g_hookedPids.insert(launchedPid);
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
                            PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(kProcessComboId, CBN_SELCHANGE),
                                         reinterpret_cast<LPARAM>(combo));
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
    const int labelWidth = 95;
    const int comboHeight = 24;
    const int editWidth = 40;
    const int buttonWidth = 120;
    const int rowHeight = 28;
    const int checkboxHeight = 20;
    const int checkboxWidth = 20;
    const int statusHeight = comboHeight * 2;

    int x = padding;
    int y = padding;

    int comboWidth = static_cast<int>(rc.right) - labelWidth - buttonWidth - padding * 3 + 5;
    if (comboWidth < 0) {
        comboWidth = 0;
    }
    SetWindowPos(GetDlgItem(hwnd, kProcessComboId), nullptr, x + labelWidth, y, comboWidth, comboHeight, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, kRefreshButtonId), nullptr, rc.right - buttonWidth - padding, y, buttonWidth, comboHeight,
                 SWP_NOZORDER);
    if (g_processLabel) {
        SetWindowPos(g_processLabel, nullptr, x, y + 2, labelWidth, comboHeight, SWP_NOZORDER);
    }

    y += rowHeight;
    if (g_speedLabel) {
        SetWindowPos(g_speedLabel, nullptr, x, y + 2, labelWidth, comboHeight, SWP_NOZORDER);
    }
    SetWindowPos(GetDlgItem(hwnd, kSpeedEditId), nullptr, x + labelWidth, y, editWidth, comboHeight, SWP_NOZORDER);
    // Place ignore-BGM label and checkbox together
    SetWindowPos(GetDlgItem(hwnd, kIgnoreBgmLabelId), nullptr, x + labelWidth + editWidth + padding, y + 2, 90, checkboxHeight,
                 SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, kIgnoreBgmCheckId), nullptr, x + labelWidth + editWidth + padding + 94, y, checkboxWidth, checkboxHeight,
                 SWP_NOZORDER);
    if (g_autoHookLabel) {
        const int autoHookGap = 10;
        const int innerGap = 6;
        const int delayGap = 10;
        const int autoHookX = x + labelWidth + editWidth + padding + 94 + checkboxWidth + autoHookGap;
        const int autoHookLabelWidth = 130;
        SetWindowPos(g_autoHookLabel, nullptr, autoHookX, y + 2, autoHookLabelWidth, checkboxHeight, SWP_NOZORDER);
        if (g_autoHookCheck) {
            const int autoHookCheckX = autoHookX + autoHookLabelWidth + innerGap;
            SetWindowPos(g_autoHookCheck, nullptr, autoHookCheckX, y, checkboxWidth, checkboxHeight, SWP_NOZORDER);
            if (g_autoHookDelayLabel) {
                const int delayLabelWidth = 125;
                const int delayCheckX = rc.right - padding - checkboxWidth;
                const int delayLabelX = delayCheckX - delayGap - delayLabelWidth;
                SetWindowPos(g_autoHookDelayLabel, nullptr, delayLabelX, y + 2, delayLabelWidth, checkboxHeight, SWP_NOZORDER);
                if (g_autoHookDelayCheck) {
                    SetWindowPos(g_autoHookDelayCheck, nullptr, delayCheckX, y, checkboxWidth, checkboxHeight, SWP_NOZORDER);
                }
            }
        }
    }
    // Hook + Apply button removed; "Hook" button handles injection.

    y += rowHeight;
    SetWindowPos(GetDlgItem(hwnd, kStatusLabelId), nullptr, x, y, rc.right - padding * 2, statusHeight, SWP_NOZORDER);

    const int linkPadding = 8;
    const int comboX = rc.right - buttonWidth - padding;
    const int comboY = rc.bottom - padding - comboHeight;
    const int hotkeyWidth = buttonWidth / 2;
    const int hotkeyX = comboX - linkPadding - hotkeyWidth;
    int linkWidth = hotkeyX - x - linkPadding;
    if (linkWidth < 0) linkWidth = 0;

    int textHeight = 0;
    if (g_link) {
        textHeight = std::max(textHeight, getTextHeight(g_link, comboHeight));
    }
    if (g_hotkeyLabel) {
        textHeight = std::max(textHeight, getTextHeight(g_hotkeyLabel, comboHeight));
    }
    if (textHeight <= 0) {
        textHeight = comboHeight;
    }
    const int textBottomY = rc.bottom - padding - textHeight;

    if (g_link) {
        SetWindowPos(g_link, nullptr, x, textBottomY, linkWidth, textHeight, SWP_NOZORDER);
    }
    if (g_hotkeyLabel) {
        SetWindowPos(g_hotkeyLabel, nullptr, hotkeyX, textBottomY, hotkeyWidth, textHeight, SWP_NOZORDER);
    }
    if (g_languageCombo) {
        const int dropHeight = comboHeight * 3; // selection + 2 items
        SetWindowPos(g_languageCombo, nullptr, comboX, comboY, buttonWidth, dropHeight, SWP_NOZORDER);
    }
}

int getTextHeight(HWND hwnd, int fallback) {
    if (!hwnd) return fallback;
    HDC hdc = GetDC(hwnd);
    if (!hdc) return fallback;
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = nullptr;
    if (font) {
        oldFont = SelectObject(hdc, font);
    }
    TEXTMETRICW tm{};
    int height = fallback;
    if (GetTextMetricsW(hdc, &tm)) {
        height = tm.tmHeight + tm.tmExternalLeading;
    }
    if (oldFont) {
        SelectObject(hdc, oldFont);
    }
    ReleaseDC(hwnd, hdc);
    return height;
}

HFONT createHotkeyFont(HWND reference) {
    if (!reference) return nullptr;
    HFONT baseFont = reinterpret_cast<HFONT>(SendMessageW(reference, WM_GETFONT, 0, 0));
    if (!baseFont) {
        baseFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
    LOGFONTW lf{};
    if (baseFont && GetObjectW(baseFont, sizeof(lf), &lf) != 0) {
        HDC hdc = GetDC(reference);
        const int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;
        if (hdc) {
            ReleaseDC(reference, hdc);
        }
        const int delta = MulDiv(2, dpi, 72);
        int height = lf.lfHeight;
        if (height == 0) {
            height = -MulDiv(11, dpi, 72);
        } else {
            if (height < 0) height = -height;
            height += delta;
            height = -height;
        }
        lf.lfHeight = height;
        lf.lfWeight = FW_BOLD;
        return CreateFontIndirectW(&lf);
    }
    return nullptr;
}

HWND createTooltip(HWND parent) {
    HWND tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                   WS_POPUP | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                   parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (tooltip) {
        SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, 360);
        SendMessageW(tooltip, TTM_ACTIVATE, TRUE, 0);
        SetWindowPos(tooltip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        KRKR_LOG_WARN("Tooltip create failed err=" + std::to_string(GetLastError()));
    }
    return tooltip;
}

TOOLINFOW makeToolInfo(HWND control, UiTextId textId) {
    TOOLINFOW ti{};
    #ifdef TTTOOLINFOW_V1_SIZE
    ti.cbSize = TTTOOLINFOW_V1_SIZE;
    #else
    ti.cbSize = sizeof(ti);
    #endif
    ti.uFlags = TTF_IDISHWND | TTF_TRACK | TTF_ABSOLUTE;
    ti.hwnd = GetParent(control);
    ti.uId = reinterpret_cast<UINT_PTR>(control);
    ti.lpszText = const_cast<wchar_t *>(ui_text::UiText(textId).c_str());
    return ti;
}

void addTooltip(HWND tooltip, HWND control, UiTextId textId) {
    if (!tooltip || !control) return;

    const auto key = reinterpret_cast<std::uintptr_t>(control);
    g_tooltipById[key] = textId;

    TOOLINFOW ti = makeToolInfo(control, textId);
    if (!SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti))) {
        KRKR_LOG_WARN("Tooltip add failed for control=" + std::to_string(key) +
                      " err=" + std::to_string(GetLastError()));
    }
}

void updateTooltips() {
    if (!g_tooltip) return;
    for (const auto &kv : g_tooltipById) {
        HWND control = reinterpret_cast<HWND>(kv.first);
        if (!IsWindow(control)) {
            continue;
        }
        TOOLINFOW ti = makeToolInfo(control, kv.second);
        SendMessageW(g_tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
    }
}

void updateTrackedTooltip(const MSG &msg) {
    if (!g_tooltip || msg.hwnd == g_tooltip) return;
    if (msg.message == WM_MOUSELEAVE) {
        if (g_activeTooltipControl && msg.hwnd == g_activeTooltipControl) {
            auto it = g_tooltipById.find(reinterpret_cast<std::uintptr_t>(g_activeTooltipControl));
            if (it != g_tooltipById.end()) {
                TOOLINFOW ti = makeToolInfo(g_activeTooltipControl, it->second);
                SendMessageW(g_tooltip, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti));
            }
            g_activeTooltipControl = nullptr;
        }
        return;
    }
    if (msg.message < WM_MOUSEFIRST || msg.message > WM_MOUSELAST) {
        return;
    }

    HWND target = nullptr;
    if (g_mainWindow) {
        HWND hit = WindowFromPoint(msg.pt);
        if (hit == g_tooltip) {
            hit = nullptr;
        }
        if (!hit || hit == g_mainWindow) {
            POINT clientPt = msg.pt;
            ScreenToClient(g_mainWindow, &clientPt);
            hit = ChildWindowFromPointEx(g_mainWindow, clientPt, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
            if (hit == g_mainWindow) {
                hit = nullptr;
            }
        }
        target = hit ? hit : msg.hwnd;
    } else {
        target = msg.hwnd;
    }
    while (target && target != g_mainWindow) {
        if (g_tooltipById.find(reinterpret_cast<std::uintptr_t>(target)) != g_tooltipById.end()) {
            break;
        }
        target = GetParent(target);
    }
    if (target == g_mainWindow) {
        target = nullptr;
    }

    if (target != g_activeTooltipControl) {
        if (g_activeTooltipControl) {
            auto itPrev = g_tooltipById.find(reinterpret_cast<std::uintptr_t>(g_activeTooltipControl));
            if (itPrev != g_tooltipById.end()) {
                TOOLINFOW ti = makeToolInfo(g_activeTooltipControl, itPrev->second);
                SendMessageW(g_tooltip, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti));
            }
        }
        g_activeTooltipControl = target;
        if (!g_activeTooltipControl) {
            return;
        }
    }

    if (g_activeTooltipControl) {
        auto it = g_tooltipById.find(reinterpret_cast<std::uintptr_t>(g_activeTooltipControl));
        if (it == g_tooltipById.end()) {
            return;
        }
        TOOLINFOW ti = makeToolInfo(g_activeTooltipControl, it->second);
        SendMessageW(g_tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
        const int x = msg.pt.x + 12;
        const int y = msg.pt.y + 18;
        SendMessageW(g_tooltip, TTM_TRACKPOSITION, 0, MAKELPARAM(x, y));
        SendMessageW(g_tooltip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&ti));

        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = g_activeTooltipControl;
        TrackMouseEvent(&tme);
    }
}

bool registerHotkey(HWND hwnd, int id, UINT vk) {
    if (!RegisterHotKey(hwnd, id, kHotkeyModifiers, vk)) {
        KRKR_LOG_WARN("RegisterHotKey failed id=" + std::to_string(id) + " err=" +
                      std::to_string(GetLastError()));
        return false;
    }
    return true;
}

void registerControllerHotkeys(HWND hwnd, HWND statusLabel) {
    bool ok = true;
    ok &= registerHotkey(hwnd, kHotkeyToggleSpeedId, VK_OEM_7); // '
    ok &= registerHotkey(hwnd, kHotkeySpeedUpId, VK_OEM_6); // ]
    ok &= registerHotkey(hwnd, kHotkeySpeedDownId, VK_OEM_4); // [
    if (!ok && statusLabel) {
        setStatus(statusLabel, L"Failed to register one or more hotkeys.");
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_mainWindow = hwnd;
        ensureUiTextLoaded();
        RECT rcClient{};
        GetClientRect(hwnd, &rcClient);
        int initialWidth = rcClient.right - 120 - 120 - 12 * 3;
        HWND combo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", nullptr,
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
                                     132, 10, initialWidth, 200, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProcessComboId)), nullptr, nullptr);
        HWND refresh = CreateWindowExW(0, L"BUTTON", ui_text::UiText(UiTextId::ButtonHook).c_str(),
                                       WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                       0, 10, 100, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshButtonId)), nullptr, nullptr);

        g_processLabel = CreateWindowExW(0, L"STATIC", ui_text::UiText(UiTextId::LabelProcess).c_str(),
                                         WS_CHILD | WS_VISIBLE | SS_NOTIFY, 12, 12, 100, 20, hwnd, nullptr, nullptr, nullptr);
        g_speedLabel = CreateWindowExW(0, L"STATIC", ui_text::UiText(UiTextId::LabelSpeed).c_str(),
                                       WS_CHILD | WS_VISIBLE | SS_NOTIFY, 12, 68, 100, 20, hwnd, nullptr, nullptr, nullptr);
        wchar_t speedText[32] = {};
        swprintf_s(speedText, L"%.2f", g_state.speed.currentSpeed);
        HWND speedEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", speedText,
                                         WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                         140, 66, 40, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSpeedEditId)), nullptr, nullptr);
        if (speedEdit && !g_speedEditProc) {
            g_speedEditProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(speedEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SpeedEditProc)));
        }
        HWND ignoreBgm = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                         140 + 40 + 12 + 90, 66, 20, 20, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIgnoreBgmCheckId)), nullptr, nullptr);
        g_ignoreBgmLabel = CreateWindowExW(0, L"STATIC", ui_text::UiText(UiTextId::LabelProcessBgm).c_str(),
                                           WS_CHILD | WS_VISIBLE | SS_NOTIFY,
                                           140 + 40 + 12, 66, 90, 20, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIgnoreBgmLabelId)), nullptr, nullptr);
        g_autoHookLabel = CreateWindowExW(0, L"STATIC", ui_text::UiText(UiTextId::LabelAutoHook).c_str(),
                                          WS_CHILD | WS_VISIBLE | SS_NOTIFY,
                                          140 + 40 + 12 + 90 + 20 + 12, 66, 135, 20,
                                          hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAutoHookLabelId)), nullptr, nullptr);
        g_autoHookCheck = CreateWindowExW(0, L"BUTTON", L"",
                                          WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                          140 + 40 + 12 + 90 + 20 + 12 + 135 + 6, 66, 20, 20,
                                          hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAutoHookCheckId)), nullptr, nullptr);
        g_autoHookDelayLabel = CreateWindowExW(0, L"STATIC", ui_text::UiText(UiTextId::LabelAutoHookDelay).c_str(),
                                               WS_CHILD | WS_VISIBLE | SS_NOTIFY,
                                               140 + 40 + 12 + 90 + 20 + 12 + 135 + 6 + 20 + 12, 66, 140, 20,
                                               hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAutoHookDelayLabelId)), nullptr, nullptr);
        g_autoHookDelayCheck = CreateWindowExW(0, L"BUTTON", L"",
                                               WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                               140 + 40 + 12 + 90 + 20 + 12 + 135 + 6 + 20 + 12 + 140 + 6, 66, 20, 20,
                                               hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAutoHookDelayCheckId)), nullptr, nullptr);
        g_languageCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", nullptr,
                                          WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                          0, 66, 120, 200,
                                          hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLanguageComboId)), nullptr, nullptr);
        if (g_languageCombo) {
            SendMessageW(g_languageCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English"));
            SendMessageW(g_languageCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"中文 Chinese"));
            SendMessageW(g_languageCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"日本語 Japanese"));
            SendMessageW(g_languageCombo, CB_SETCURSEL, 1, 0);
        }
        if (ignoreBgm) {
            SendMessageW(ignoreBgm, BM_SETCHECK, g_state.processAllAudio ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        // Hook + Apply button removed; "Hook" button handles injection.

        CreateWindowExW(0, L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE,
                        12, 96, 400, 20, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusLabelId)), nullptr, nullptr);

        g_linkIsSysLink = true;
        const wchar_t *linkText = ui_text::UiText(UiTextId::LinkMarkup).c_str();
        g_link = CreateWindowExW(0, WC_LINK, linkText,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 12, 124, 500, 24,
                                 hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLinkId)), nullptr, nullptr);
        if (!g_link) {
            // Fallback for systems without SysLink (e.g., missing comctl32 v6 manifest).
            g_linkIsSysLink = false;
            const wchar_t *fallbackText = ui_text::UiText(UiTextId::LinkPlain).c_str();
            g_link = CreateWindowExW(0, L"STATIC", fallbackText,
                                     WS_CHILD | WS_VISIBLE | SS_NOTIFY,
                                     12, 124, 500, 20,
                                     hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLinkId)), nullptr, nullptr);
        }
        if (g_link) {
            SendMessageW(g_link, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        }
        g_hotkeyLabel = CreateWindowExW(0, L"STATIC", ui_text::UiText(UiTextId::LabelHotkey).c_str(),
                                        WS_CHILD | WS_VISIBLE | SS_NOTIFY,
                                        12, 124, 60, 24, hwnd, nullptr, nullptr, nullptr);
        if (g_hotkeyLabel) {
            if (g_hotkeyFont) {
                DeleteObject(g_hotkeyFont);
                g_hotkeyFont = nullptr;
            }
            g_hotkeyFont = createHotkeyFont(g_processLabel ? g_processLabel : g_hotkeyLabel);
            HFONT labelFont = g_hotkeyFont;
            if (!labelFont) {
                labelFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            }
            SendMessageW(g_hotkeyLabel, WM_SETFONT, reinterpret_cast<WPARAM>(labelFont), TRUE);
        }

        layoutControls(hwnd);
        refreshProcessList(combo, GetDlgItem(hwnd, kStatusLabelId));
        updateAutoHookCheckbox(hwnd);
        updateProcessBgmCheckbox(hwnd);
        updateHookButtonState(hwnd);
        initKnownPids();
        scheduleAutoHooksForProcesses(hwnd, controller::enumerateSessionProcesses(), true);
        SetTimer(hwnd, kAutoHookTimerId, kAutoHookIntervalMs, nullptr);
        registerControllerHotkeys(hwnd, GetDlgItem(hwnd, kStatusLabelId));

        g_tooltip = createTooltip(hwnd);
        addTooltip(g_tooltip, combo, UiTextId::TooltipProcessCombo);
        addTooltip(g_tooltip, g_processLabel, UiTextId::TooltipProcessCombo);
        addTooltip(g_tooltip, refresh, UiTextId::TooltipHookButton);
        addTooltip(g_tooltip, speedEdit, UiTextId::TooltipSpeedEdit);
        addTooltip(g_tooltip, g_speedLabel, UiTextId::TooltipSpeedEdit);
        addTooltip(g_tooltip, ignoreBgm, UiTextId::TooltipProcessBgm);
        addTooltip(g_tooltip, g_ignoreBgmLabel, UiTextId::TooltipProcessBgm);
        addTooltip(g_tooltip, g_autoHookCheck, UiTextId::TooltipAutoHook);
        addTooltip(g_tooltip, g_autoHookLabel, UiTextId::TooltipAutoHook);
        addTooltip(g_tooltip, g_autoHookDelayCheck, UiTextId::TooltipAutoHookDelay);
        addTooltip(g_tooltip, g_autoHookDelayLabel, UiTextId::TooltipAutoHookDelay);
        addTooltip(g_tooltip, g_hotkeyLabel, UiTextId::TooltipHotkey);
        // Hook + Apply tooltip removed with button.

        refreshUiText(hwnd);

        if (!g_initialOptions.launchPath.empty()) {
            handleLaunch(hwnd);
        }

        // Auto search & hook if requested via CLI.
        if (!g_state.searchTerm.empty()) {
            auto toLower = [](std::wstring s) {
                std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                return s;
            };
            const auto needle = toLower(g_state.searchTerm);
            int bestIdx = -1;
            size_t bestLen = std::numeric_limits<size_t>::max();
            for (size_t i = 0; i < g_state.processes.size(); ++i) {
                auto nameLower = toLower(g_state.processes[i].name);
                if (nameLower.find(needle) != std::wstring::npos) {
                    const size_t len = g_state.processes[i].name.length();
                    if (len < bestLen) {
                        bestLen = len;
                        bestIdx = static_cast<int>(i);
                    }
                }
            }

            HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);
            if (bestIdx >= 0) {
                SendMessageW(combo, CB_SETCURSEL, bestIdx, 0);
                updateAutoHookCheckbox(hwnd);
                updateProcessBgmCheckbox(hwnd);
                updateHookButtonState(hwnd);
                const auto &p = g_state.processes[static_cast<size_t>(bestIdx)];
                std::wstring msg = L"Auto-selected [" + std::to_wstring(p.pid) + L"] " + p.name + L" via --search \"" + g_state.searchTerm + L"\"";
                setStatus(statusLabel, msg);
                KRKR_LOG_INFO(L"Auto search hit: " + p.name);
                handleApply(hwnd);
            } else {
                std::wstring msg = L"--search \"" + g_state.searchTerm + L"\": no process matched; waiting for manual selection.";
                setStatus(statusLabel, msg);
                KRKR_LOG_INFO(L"Search term not found: " + g_state.searchTerm);
            }
        }
        break;
    }
    case WM_SIZE:
        layoutControls(hwnd);
        break;
    case WM_COMMAND: {
        const WORD id = LOWORD(wParam);
        if (id == kRefreshButtonId && HIWORD(wParam) == BN_CLICKED) {
            handleApply(hwnd);
        } else if (id == kProcessComboId && HIWORD(wParam) == CBN_DROPDOWN) {
            refreshProcessUi(hwnd, GetDlgItem(hwnd, kProcessComboId), GetDlgItem(hwnd, kStatusLabelId));
        } else if (id == kProcessComboId && HIWORD(wParam) == CBN_SELCHANGE) {
            updateAutoHookCheckbox(hwnd);
            updateProcessBgmCheckbox(hwnd);
            updateHookButtonState(hwnd);
        } else if (id == kLanguageComboId && HIWORD(wParam) == CBN_SELCHANGE) {
            const int sel = static_cast<int>(SendMessageW(g_languageCombo, CB_GETCURSEL, 0, 0));
            if (sel == 1) {
                ui_text::SetUiLanguage(L"zh-CN");
            } else if (sel == 2) {
                ui_text::SetUiLanguage(L"ja");
            } else {
                ui_text::SetUiLanguage(L"en");
            }
            refreshUiText(hwnd);
        } else if (id == kSpeedEditId && HIWORD(wParam) == EN_KILLFOCUS) {
            HWND editSpeed = GetDlgItem(hwnd, kSpeedEditId);
            if (editSpeed) {
                readSpeedFromEdit(editSpeed);
                writeSpeedEdit(hwnd);
                applySettingsToSelectedIfHooked(hwnd);
            }
        } else if (id == kIgnoreBgmCheckId && HIWORD(wParam) == BN_CLICKED) {
            syncProcessAllAudioFromCheckbox(hwnd);
            handleProcessBgmToggle(hwnd);
            applySettingsToSelectedIfHooked(hwnd);
        } else if (id == kAutoHookCheckId && HIWORD(wParam) == BN_CLICKED) {
            handleAutoHookToggle(hwnd);
        } else if (id == kAutoHookDelayCheckId && HIWORD(wParam) == BN_CLICKED) {
            handleAutoHookDelayToggle(hwnd);
        } else if (id == kLinkId && HIWORD(wParam) == STN_CLICKED) {
            ShellExecuteW(hwnd, L"open", L"https://github.com/caca2331/kirikiri-speed-controller", nullptr, nullptr, SW_SHOWNORMAL);
        }
        break;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        HWND speedEdit = GetDlgItem(hwnd, kSpeedEditId);
        if (speedEdit && GetFocus() == speedEdit) {
            SetFocus(hwnd);
        }
        break;
    }
    case WM_TIMER:
        if (wParam == kAutoHookTimerId) {
            pollAutoHook(hwnd);
            return 0;
        }
        break;
    case WM_HOTKEY: {
        HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);
        HWND editSpeed = GetDlgItem(hwnd, kSpeedEditId);
        if (editSpeed) {
            readSpeedFromEdit(editSpeed);
        }
        ProcessInfo proc;
        std::wstring error;
        if (!getSelectedProcess(hwnd, proc, error)) {
            setStatus(statusLabel, error);
            return 0;
        }
        syncProcessAllAudioFromCheckbox(hwnd);
        controller::SharedConfig baseCfg = buildSharedConfig(g_state.speed.currentSpeed);
        std::wstring status;
        if (wParam == kHotkeyToggleSpeedId) {
            if (!controller::applySpeedHotkey(proc.pid, baseCfg, g_state.speed, SpeedHotkeyAction::Toggle, status, error)) {
                setStatus(statusLabel, error);
                return 0;
            }
        } else if (wParam == kHotkeySpeedUpId) {
            if (!controller::applySpeedHotkey(proc.pid, baseCfg, g_state.speed, SpeedHotkeyAction::SpeedUp, status, error)) {
                setStatus(statusLabel, error);
                return 0;
            }
        } else if (wParam == kHotkeySpeedDownId) {
            if (!controller::applySpeedHotkey(proc.pid, baseCfg, g_state.speed, SpeedHotkeyAction::SpeedDown, status, error)) {
                setStatus(statusLabel, error);
                return 0;
            }
        } else {
            break;
        }
        writeSpeedEdit(hwnd);
        setStatus(statusLabel, status);
        return 0;
    }
    case kMsgRefreshQuiet: {
        refreshProcessList(GetDlgItem(hwnd, kProcessComboId), GetDlgItem(hwnd, kStatusLabelId), true);
        return 0;
    }
    case kMsgAutoSelectPid: {
        DWORD pid = static_cast<DWORD>(wParam);
        g_pendingAutoSelectPid = pid;
        g_pendingAutoHookRefresh = true;
        g_hookedPids.insert(pid);
        HWND combo = GetDlgItem(hwnd, kProcessComboId);
        if (combo) {
            refreshProcessUi(hwnd, combo, GetDlgItem(hwnd, kStatusLabelId));
            if (selectProcessByPid(hwnd, pid)) {
                g_pendingAutoSelectPid = 0;
                g_pendingAutoHookRefresh = false;
            }
        }
        return 0;
    }
    case WM_NOTIFY: {
        auto *hdr = reinterpret_cast<NMHDR *>(lParam);
        if (hdr && hdr->idFrom == kLinkId && (hdr->code == NM_CLICK || hdr->code == NM_RETURN)) {
            ShellExecuteW(hwnd, L"open", L"https://github.com/caca2331/kirikiri-speed-controller", nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        break;
    }
    case WM_DESTROY:
        KillTimer(hwnd, kAutoHookTimerId);
        UnregisterHotKey(hwnd, kHotkeyToggleSpeedId);
        UnregisterHotKey(hwnd, kHotkeySpeedUpId);
        UnregisterHotKey(hwnd, kHotkeySpeedDownId);
        g_activeTooltipControl = nullptr;
        g_mainWindow = nullptr;
        if (g_hotkeyFont) {
            DeleteObject(g_hotkeyFont);
            g_hotkeyFont = nullptr;
        }
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
    controller::initSpeedState(g_state.speed, opts.speed, true);
    g_state.bgmSeconds = opts.bgmSeconds;
    g_state.launchPath = opts.launchPath.empty() ? std::filesystem::path{} : std::filesystem::path(opts.launchPath);
    g_state.stereoBgmMode = opts.stereoBgmMode;
    g_state.searchTerm = opts.searchTerm;
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

    ensureUiTextLoaded();
    const auto &title = ui_text::UiText(UiTextId::WindowTitle);
    HWND hwnd = CreateWindowExW(0, CLASS_NAME, title.c_str(),
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
        updateTrackedTooltip(msg);
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

} // namespace krkrspeed::ui
