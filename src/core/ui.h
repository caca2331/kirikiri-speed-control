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
    bool skipFmod = false;
    bool skipWwise = false;
    bool safeMode = false;
    bool processAllAudio = false;
    float speed = 1.5f;
    float bgmSeconds = 60.0f;
    std::wstring launchPath;
    std::uint32_t stereoBgmMode = 1;
    std::wstring searchTerm;
};

void setInitialOptions(const ControllerOptions &opts);
ControllerOptions getInitialOptions();

} // namespace krkrspeed::ui
