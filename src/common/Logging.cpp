#include "Logging.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

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

void ensureOpen(LoggerState &stateRef) {
    if (stateRef.initialized) {
        return;
    }
    stateRef.initialized = true;

    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec);
    if (ec) {
        return;
    }

    const auto pid = currentProcessId();
    auto path = dir / ("krkr_speed_" + std::to_string(pid) + ".log");
    stateRef.stream.open(path, std::ios::out | std::ios::app);
    if (stateRef.stream.is_open()) {
        stateRef.path = path.string();
        stateRef.stream << "----- log start " << currentTimestamp() << " (pid " << pid << ") -----" << std::endl;
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
