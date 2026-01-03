#include "SharedSettingsManager.h"
#include "../common/Logging.h"

#include <algorithm>
#include <cmath>

namespace krkrspeed {

SharedSettingsManager &SharedSettingsManager::instance() {
    static SharedSettingsManager mgr;
    return mgr;
}

void SharedSettingsManager::attachSharedSettings() {
    if (m_sharedView) {
        return;
    }
    const auto name = BuildSharedSettingsName(static_cast<std::uint32_t>(GetCurrentProcessId()));
    m_sharedMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!m_sharedMapping) {
        if (!m_warnedMissingMap.exchange(true)) {
            KRKR_LOG_WARN("Shared settings map not found; using defaults");
        }
        return;
    }
    m_sharedView = static_cast<SharedSettings *>(MapViewOfFile(m_sharedMapping, FILE_MAP_READ, 0, 0, sizeof(SharedSettings)));
    if (!m_sharedView) {
        KRKR_LOG_WARN("MapViewOfFile failed for shared settings");
        CloseHandle(m_sharedMapping);
        m_sharedMapping = nullptr;
        return;
    }
    KRKR_LOG_INFO("Attached to shared settings map");
}

void SharedSettingsManager::applySharedSettings(const SharedSettings &settings) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const float newSpeed = std::clamp(settings.userSpeed, 0.5f, 10.0f);
    const float newGateSeconds = std::clamp(settings.lengthGateSeconds, 0.1f, 600.0f);
    const bool gateEnabled = settings.lengthGateEnabled != 0;
    const bool speedChanged = std::fabs(newSpeed - m_userSpeed) > 0.001f;
    const bool gateChanged =
        gateEnabled != m_lengthGateEnabled || std::fabs(newGateSeconds - m_lengthGateSeconds) > 0.001f;

    m_userSpeed = newSpeed;
    m_lengthGateEnabled = gateEnabled;
    m_lengthGateSeconds = newGateSeconds;

    if (speedChanged) {
        m_speedChangeCounter.fetch_add(1);
        KRKR_LOG_INFO("Shared speed updated to " + std::to_string(m_userSpeed) + "x");
    }
    if (gateChanged) {
        KRKR_LOG_INFO(std::string("Shared length gate ") + (m_lengthGateEnabled ? "enabled" : "disabled") +
                      " @ " + std::to_string(m_lengthGateSeconds) + "s");
    }
}

void SharedSettingsManager::pollSharedSettings() {
    if (!m_sharedView) {
        attachSharedSettings();
        if (!m_sharedView) {
            return;
        }
    }
    SharedSettings snapshot = *m_sharedView;
    applySharedSettings(snapshot);
}

float SharedSettingsManager::getUserSpeed() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_userSpeed;
}

bool SharedSettingsManager::isLengthGateEnabled() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lengthGateEnabled;
}

float SharedSettingsManager::lengthGateSeconds() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lengthGateSeconds;
}

} // namespace krkrspeed
