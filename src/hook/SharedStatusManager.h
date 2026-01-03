#pragma once

#include "../common/SharedStatus.h"
#include <Windows.h>
#include <atomic>
#include <mutex>

namespace krkrspeed {

class SharedStatusManager {
public:
    static SharedStatusManager &instance();

    void setActiveBackend(AudioBackend backend);

private:
    SharedStatusManager() = default;
    void ensureMapping();

    std::mutex m_mutex;
    HANDLE m_mapping = nullptr;
    SharedStatus *m_view = nullptr;
    AudioBackend m_lastBackend = AudioBackend::Unknown;
    std::atomic<bool> m_warned{false};
};

} // namespace krkrspeed
