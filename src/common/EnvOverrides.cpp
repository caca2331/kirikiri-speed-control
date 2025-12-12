#include "EnvOverrides.h"

#include <Windows.h>
#include <fstream>
#include <sstream>

namespace krkrspeed {

std::filesystem::path EnvOverrideFilePath() {
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec);
    if (ec) return {};
    return dir / "krkr_env_overrides.txt";
}

void WriteEnvOverrides(const std::vector<EnvPair> &overrides) {
    if (overrides.empty()) return;
    const auto path = EnvOverrideFilePath();
    if (path.empty()) return;

    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out) return;
    for (const auto &kv : overrides) {
        std::wstring key = kv.first;
        std::wstring val = kv.second;
        // Convert to UTF-8 for persistence.
        int klen = WideCharToMultiByte(CP_UTF8, 0, key.c_str(), static_cast<int>(key.size()), nullptr, 0, nullptr, nullptr);
        int vlen = WideCharToMultiByte(CP_UTF8, 0, val.c_str(), static_cast<int>(val.size()), nullptr, 0, nullptr, nullptr);
        if (klen <= 0) continue;
        std::string k8(static_cast<size_t>(klen), '\0');
        WideCharToMultiByte(CP_UTF8, 0, key.c_str(), static_cast<int>(key.size()), k8.data(), klen, nullptr, nullptr);
        std::string v8;
        if (vlen > 0) {
            v8.resize(static_cast<size_t>(vlen));
            WideCharToMultiByte(CP_UTF8, 0, val.c_str(), static_cast<int>(val.size()), v8.data(), vlen, nullptr, nullptr);
        }
        out << k8 << "=" << v8 << "\n";
    }
}

void ApplyEnvOverridesFromFile() {
    const auto path = EnvOverrideFilePath();
    if (path.empty() || !std::filesystem::exists(path)) return;

    std::ifstream in(path);
    if (!in) return;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // Convert to UTF-16 and set environment variable for current process.
        int klen = MultiByteToWideChar(CP_UTF8, 0, key.c_str(), static_cast<int>(key.size()), nullptr, 0);
        int vlen = MultiByteToWideChar(CP_UTF8, 0, val.c_str(), static_cast<int>(val.size()), nullptr, 0);
        if (klen <= 0) continue;
        std::wstring wkey(static_cast<size_t>(klen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, key.c_str(), static_cast<int>(key.size()), wkey.data(), klen);
        std::wstring wval;
        if (vlen > 0) {
            wval.resize(static_cast<size_t>(vlen), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, val.c_str(), static_cast<int>(val.size()), wval.data(), vlen);
        }
        SetEnvironmentVariableW(wkey.c_str(), wval.empty() ? nullptr : wval.c_str());
    }
}

} // namespace krkrspeed
