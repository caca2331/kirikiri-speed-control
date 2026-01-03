#include "SharedStatusManager.h"
#include "../common/Logging.h"

namespace krkrspeed {

SharedStatusManager &SharedStatusManager::instance() {
    static SharedStatusManager mgr;
    return mgr;
}

void SharedStatusManager::ensureMapping() {
    if (m_view) {
        return;
    }
    const auto name = BuildSharedStatusName(static_cast<std::uint32_t>(GetCurrentProcessId()));
    m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedStatus), name.c_str());
    if (!m_mapping) {
        if (!m_warned.exchange(true)) {
            KRKR_LOG_WARN("Shared status map create failed; status UI will be unavailable");
        }
        return;
    }
    m_view = static_cast<SharedStatus *>(MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedStatus)));
    if (!m_view) {
        if (!m_warned.exchange(true)) {
            KRKR_LOG_WARN("MapViewOfFile failed for shared status map");
        }
        CloseHandle(m_mapping);
        m_mapping = nullptr;
        return;
    }
}

void SharedStatusManager::setActiveBackend(AudioBackend backend) {
    if (backend == AudioBackend::Unknown) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (backend == m_lastBackend) {
        return;
    }
    ensureMapping();
    if (!m_view) {
        return;
    }
    m_view->activeBackend = static_cast<std::uint32_t>(backend);
    m_view->lastUpdateMs = GetTickCount64();
    m_lastBackend = backend;
}

} // namespace krkrspeed
