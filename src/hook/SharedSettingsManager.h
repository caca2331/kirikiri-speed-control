#pragma once

#include "../common/SharedSettings.h"
#include <Windows.h>
#include <atomic>
#include <mutex>

namespace krkrspeed {

class SharedSettingsManager {
public:
    static SharedSettingsManager &instance();

    void attachSharedSettings();
    void pollSharedSettings();
    void applySharedSettings(const SharedSettings &settings);

    float getUserSpeed() const;
    bool isLengthGateEnabled() const;
    float lengthGateSeconds() const;
    std::uint64_t speedChangeCounter() const { return m_speedChangeCounter.load(); }

private:
    SharedSettingsManager() = default;

    mutable std::mutex m_mutex;
    float m_userSpeed = 1.5f;
    bool m_lengthGateEnabled = true;
    float m_lengthGateSeconds = 60.0f;

    HANDLE m_sharedMapping = nullptr;
    SharedSettings *m_sharedView = nullptr;
    std::atomic<std::uint64_t> m_speedChangeCounter{0};
    std::atomic<bool> m_warnedMissingMap{false};
};

} // namespace krkrspeed
