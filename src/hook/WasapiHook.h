#pragma once

#include <Windows.h>
#include <atomic>

namespace krkrspeed {

class WasapiHook {
public:
    static WasapiHook &instance();
    void initialize();
    void tryPatchModule(HMODULE module, const char *moduleName);
    void setOriginalCoCreate(void *fn);

    static HRESULT WINAPI CoCreateInstanceHook(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext,
                                               REFIID riid, LPVOID *ppv);

private:
    WasapiHook() = default;

    using PFN_CoCreateInstance = HRESULT(WINAPI *)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);

    PFN_CoCreateInstance m_origCoCreate = nullptr;
    std::atomic<bool> m_loggedInit{false};
};

} // namespace krkrspeed
