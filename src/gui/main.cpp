#include "ui.h"
#include <Windows.h>
#include <filesystem>
#include <fstream>

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    // Hint the hook DLL to log beside the controller.
    wchar_t modulePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) != 0) {
        std::filesystem::path exe(modulePath);
        auto dir = exe.parent_path();
        SetEnvironmentVariableW(L"KRKR_LOG_DIR", dir.wstring().c_str());
        std::error_code ec;
        auto hintFile = std::filesystem::temp_directory_path(ec) / "krkr_log_dir.txt";
        if (!ec) {
            std::ofstream out(hintFile);
            if (out) {
                out << dir.u8string();
            }
        }
    }
    return krkrspeed::ui::runController(hInstance, nCmdShow);
}
