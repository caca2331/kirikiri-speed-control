#pragma once

#include <string>

namespace krkrspeed {

enum class LogLevel { Debug, Info, Warn, Error };

#ifndef KRKR_ENABLE_LOGGING
#define KRKR_ENABLE_LOGGING 1
#endif

#if KRKR_ENABLE_LOGGING
void logMessage(LogLevel level, const std::string &message);
void logMessage(LogLevel level, const std::wstring &message);
#else
inline void logMessage(LogLevel, const std::string &) {}
inline void logMessage(LogLevel, const std::wstring &) {}
#endif

} // namespace krkrspeed

#if KRKR_ENABLE_LOGGING
#define KRKR_LOG_DEBUG(msg) ::krkrspeed::logMessage(::krkrspeed::LogLevel::Debug, (msg))
#define KRKR_LOG_INFO(msg) ::krkrspeed::logMessage(::krkrspeed::LogLevel::Info, (msg))
#define KRKR_LOG_WARN(msg) ::krkrspeed::logMessage(::krkrspeed::LogLevel::Warn, (msg))
#define KRKR_LOG_ERROR(msg) ::krkrspeed::logMessage(::krkrspeed::LogLevel::Error, (msg))
#else
#define KRKR_LOG_DEBUG(msg) ((void)0)
#define KRKR_LOG_INFO(msg) ((void)0)
#define KRKR_LOG_WARN(msg) ((void)0)
#define KRKR_LOG_ERROR(msg) ((void)0)
#endif
