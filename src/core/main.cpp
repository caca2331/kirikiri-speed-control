#include "ui.h"
#include "ControllerCore.h"
#include "../common/Logging.h"
#include <Windows.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>

namespace {

struct CliOptions {
    bool enableLog = false;
    float speed = 1.5f;
    float bgmSeconds = 60.0f;
    std::filesystem::path launchPath;
    std::uint32_t stereoBgmMode = 1;
    std::wstring searchTerm;
};

CliOptions parseArgs() {
    CliOptions opts;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return opts;

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        auto next = [&](std::wstring &out) -> bool {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };

        if (arg == L"--log") {
            opts.enableLog = true;
        } else if (arg == L"--bgm-secs") {
            std::wstring v;
            if (next(v)) {
                try {
                    opts.bgmSeconds = std::stof(v);
                } catch (...) {}
            }
        } else if (arg == L"--speed") {
            std::wstring v;
            if (next(v)) {
                try {
                    opts.speed = std::stof(v);
                } catch (...) {}
            }
        } else if (arg == L"--mark-stereo-bgm") {
            std::wstring v;
            if (next(v)) {
                if (_wcsicmp(v.c_str(), L"aggressive") == 0) opts.stereoBgmMode = 0;
                else if (_wcsicmp(v.c_str(), L"hybrid") == 0) opts.stereoBgmMode = 1;
                else if (_wcsicmp(v.c_str(), L"none") == 0) opts.stereoBgmMode = 2;
            }
        } else if (arg == L"--launch" || arg == L"-l") {
            std::wstring v;
            if (next(v)) {
                opts.launchPath = v;
            }
        } else if (arg == L"--search") {
            std::wstring v;
            if (next(v)) {
                opts.searchTerm = v;
            }
        }
    }

    LocalFree(argv);
    return opts;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\KrkrSpeedController_Instance");
    if (instanceMutex) {
        const DWORD mutexErr = GetLastError();
        if (mutexErr == ERROR_ALREADY_EXISTS || mutexErr == ERROR_ACCESS_DENIED) {
            CloseHandle(instanceMutex);
            return 0;
        }
    }

    auto opts = parseArgs();
    krkrspeed::controller::loadAutoHookConfig();
    krkrspeed::ui::ControllerOptions controllerOpts{};
    controllerOpts.enableLog = opts.enableLog;
    controllerOpts.speed = opts.speed;
    controllerOpts.bgmSeconds = opts.bgmSeconds;
    controllerOpts.launchPath = opts.launchPath.wstring();
    controllerOpts.stereoBgmMode = opts.stereoBgmMode;
    controllerOpts.searchTerm = opts.searchTerm;
    krkrspeed::ui::setInitialOptions(controllerOpts);
    krkrspeed::SetLoggingEnabled(opts.enableLog);

    // Hint the hook DLL to log beside the controller.
    wchar_t modulePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) != 0) {
        std::filesystem::path exe(modulePath);
        auto dir = exe.parent_path();
        std::filesystem::path chosenLogDir = dir;
        if (!chosenLogDir.empty()) {
            krkrspeed::SetLogDirectory(chosenLogDir.wstring());
        }
        std::error_code ec;
        auto hintFile = std::filesystem::temp_directory_path(ec) / "krkr_log_dir.txt";
        if (!ec) {
            std::ofstream out(hintFile);
            if (out) {
                out << chosenLogDir.u8string();
            }
        }
    }

    // Persist overrides so the injected hook process can apply them on attach.
    const int result = krkrspeed::ui::runController(hInstance, nCmdShow);
    if (instanceMutex) {
        CloseHandle(instanceMutex);
    }
    return result;
}
