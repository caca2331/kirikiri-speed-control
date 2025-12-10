#include "Logging.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <cstdlib>

#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace krkrspeed {

namespace {

struct LoggerState {
    std::mutex mutex;
    std::ofstream stream;
    std::string path;
    bool initialized = false;
};

std::string levelToString(LogLevel level) {
    switch (level) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Error: return "ERROR";
    }
    return "UNK";
}

std::string currentTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

std::string toUtf8(const std::wstring &wstr) {
#ifdef _WIN32
    if (wstr.empty()) return {};
    const int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0) return {};
    std::string result(sizeNeeded, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), result.data(), sizeNeeded, nullptr, nullptr);
    return result;
#else
    // Narrowing best-effort for non-Windows builds.
    std::string result;
    result.reserve(wstr.size());
    for (wchar_t ch : wstr) {
        result.push_back(static_cast<char>(ch));
    }
    return result;
#endif
}

unsigned long currentProcessId() {
#ifdef _WIN32
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

LoggerState &state() {
    static LoggerState instance;
    return instance;
}

std::filesystem::path moduleDirectory() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) == 0) {
        return {};
    }
    std::filesystem::path p(buffer);
    return p.parent_path();
#else
    return {};
#endif
}

void pruneOldLogs(const std::filesystem::path &dir) {
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const auto name = entry.path().filename().string();
        if (name.rfind("krkr_", 0) == 0 && entry.path().extension() == ".log") {
            std::filesystem::remove(entry.path(), ec);
        }
    }
}

std::filesystem::path readHintPath() {
    std::error_code ec;
    auto temp = std::filesystem::temp_directory_path(ec);
    if (ec) return {};
    auto hintFile = temp / "krkr_log_dir.txt";
    if (!std::filesystem::exists(hintFile)) return {};
    std::ifstream in(hintFile);
    if (!in) return {};
    std::string line;
    std::getline(in, line);
    if (line.empty()) return {};
    std::filesystem::path p(line);
    if (std::filesystem::exists(p) && std::filesystem::is_directory(p)) {
        return p;
    }
    return {};
}

std::filesystem::path executableStem() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) == 0) {
        return {};
    }
    return std::filesystem::path(buffer).stem();
#else
    return {};
#endif
}

std::filesystem::path chooseLogDirectory() {
    // 1) env var
#ifdef _WIN32
    wchar_t buf[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"KRKR_LOG_DIR", buf, MAX_PATH) > 0) {
        std::filesystem::path p(buf);
        if (std::filesystem::exists(p) && std::filesystem::is_directory(p)) return p;
    }
#endif
    // 2) hint file in temp
    auto hint = readHintPath();
    if (!hint.empty()) return hint;
    // 3) module directory
    auto mod = moduleDirectory();
    if (!mod.empty()) return mod;
    // 4) temp
    std::error_code ec;
    return std::filesystem::temp_directory_path(ec);
}

void ensureOpen(LoggerState &stateRef) {
    if (stateRef.initialized) {
        return;
    }
    stateRef.initialized = true;

    auto dir = chooseLogDirectory();
    std::error_code ec;

    auto stem = executableStem();
    std::string base = "krkr_speed";
    if (!stem.empty()) {
        auto s = stem.string();
        if (s == "KrkrSpeedController") base = "krkr_controller";
        else if (s == "krkr_speed_hook") base = "krkr_hook";
    }

    pruneOldLogs(dir);
    const auto path = dir / (base + ".log");
    std::filesystem::remove(path, ec);

    stateRef.stream.open(path, std::ios::out | std::ios::trunc);
    if (stateRef.stream.is_open()) {
        stateRef.path = path.string();
        stateRef.stream << "----- log start " << currentTimestamp() << " (pid " << currentProcessId() << ") -----"
                        << std::endl;
    }
}

void writeLine(LoggerState &stateRef, LogLevel level, const std::string &line) {
    if (!stateRef.stream.is_open()) {
        return;
    }
    stateRef.stream << "[" << currentTimestamp() << "] [" << levelToString(level) << "] " << line << std::endl;
#ifdef _WIN32
    std::string dbg = "[krkr] " + line + "\n";
    OutputDebugStringA(dbg.c_str());
#endif
}

} // namespace

void logMessage(LogLevel level, const std::string &message) {
    static bool loggingChecked = false;
    static bool loggingEnabled = false;
    if (!loggingChecked) {
#ifdef _WIN32
        wchar_t buf[8] = {};
        DWORD n = GetEnvironmentVariableW(L"KRKR_ENABLE_LOG", buf, static_cast<DWORD>(std::size(buf)));
        loggingEnabled = (n > 0 && n < std::size(buf) && wcscmp(buf, L"1") == 0);
#else
        const char *env = std::getenv("KRKR_ENABLE_LOG");
        loggingEnabled = env && env[0] == '1';
#endif
        loggingChecked = true;
    }
    if (!loggingEnabled) {
        return;
    }

    auto &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    ensureOpen(s);
    if (s.stream.is_open()) {
        writeLine(s, level, message);
    } else {
        // Fallback to stderr if the log file cannot be opened.
        std::clog << "[" << levelToString(level) << "] " << message << std::endl;
    }
}

void logMessage(LogLevel level, const std::wstring &message) {
    logMessage(level, toUtf8(message));
}

} // namespace krkrspeed
