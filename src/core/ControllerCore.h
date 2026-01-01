#pragma once

#ifndef NOMINMAX
#define NOMINMAX 1
#endif

#include <Windows.h>
#include <filesystem>
#include <string>
#include <vector>

namespace krkrspeed::controller {

enum class ProcessArch { Unknown, X86, X64, Arm64 };

struct ProcessInfo {
    std::wstring name;
    DWORD pid = 0;
    ProcessArch arch = ProcessArch::Unknown;
    bool hasWindow = false;
};

struct SharedConfig {
    float speed = 1.5f;
    bool lengthGateEnabled = true;
    float bgmSeconds = 60.0f;
    bool enableLog = false;
    bool skipDirectSound = false;
    bool skipXAudio2 = false;
    bool skipFmod = false;
    bool skipWwise = false;
    bool safeMode = false;
    bool processAllAudio = false;
    std::uint32_t stereoBgmMode = 1;
};

struct AutoHookEntry {
    std::wstring exeName;
    std::wstring exePath;
};

struct SpeedControlState {
    float currentSpeed = 1.5f;
    float lastValidSpeed = 1.5f;
    bool enabled = true;
};

enum class SpeedHotkeyAction {
    Toggle,
    SpeedUp,
    SpeedDown
};

ProcessArch getSelfArch();
bool ensureDebugPrivilege();
std::filesystem::path controllerDirectory();
std::vector<ProcessInfo> enumerateVisibleProcesses();
bool queryProcessArch(DWORD pid, ProcessArch &archOut, std::wstring &error);
bool getProcessExePath(DWORD pid, std::wstring &pathOut, std::wstring &error);
bool getDllArch(const std::filesystem::path &path, ProcessArch &archOut, std::wstring &error);
bool selectHookForArch(const std::filesystem::path &controllerDir, ProcessArch targetArch, std::filesystem::path &outPath,
                       std::wstring &error);
bool writeSharedSettingsForPid(DWORD pid, const SharedConfig &config, std::wstring &error);
bool injectDllIntoProcess(ProcessArch targetArch, DWORD pid, const std::filesystem::path &dllPath, std::wstring &error);
bool launchAndInject(const std::filesystem::path &exePath, const SharedConfig &config, DWORD &outPid, std::wstring &error);
std::wstring describeArch(ProcessArch arch);

void loadAutoHookConfig();
bool isAutoHookEnabled(const std::wstring &exePath, const std::wstring &exeName);
bool setAutoHookEnabled(const std::wstring &exePath, const std::wstring &exeName, bool enabled, std::wstring &error);

float clampSpeed(float speed);
float roundSpeed(float speed);
float effectiveSpeed(const SpeedControlState &state);
void initSpeedState(SpeedControlState &state, float speed, bool enabled = true);
void updateSpeedFromInput(SpeedControlState &state, float speed);
bool applySpeedToPid(DWORD pid, const SharedConfig &baseConfig, const SpeedControlState &state, std::wstring &error);
bool applySpeedHotkey(DWORD pid, const SharedConfig &baseConfig, SpeedControlState &state,
                      SpeedHotkeyAction action, std::wstring &status, std::wstring &error);

} // namespace krkrspeed::controller
