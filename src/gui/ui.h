#pragma once

#include <Windows.h>
#include <string>

namespace krkrspeed::ui {

// Starts the controller UI message loop. Returns the process exit code.
int runController(HINSTANCE hInstance, int nCmdShow);

struct ControllerOptions {
    bool enableLog = false;
    bool skipDirectSound = false;
    bool skipXAudio2 = false;
    bool safeMode = false;
    bool disableVeh = false;
    bool disableBgm = false;
    bool forceAll = false;
    float bgmSeconds = 60.0f;
    std::wstring launchPath;
};

void setInitialOptions(const ControllerOptions &opts);
ControllerOptions getInitialOptions();

} // namespace krkrspeed::ui
