#include "ModuleScan.h"
#include "../common/Logging.h"
#include <windows.h>
#include <Psapi.h>
#include <string>
#include <vector>

namespace krkrspeed {

std::vector<std::string> ListLoadedModules() {
    std::vector<std::string> out;
    HMODULE mods[512];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        return out;
    }
    size_t count = std::min<std::size_t>(needed / sizeof(HMODULE), std::size(mods));
    char name[MAX_PATH] = {};
    for (size_t i = 0; i < count; ++i) {
        if (GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, static_cast<DWORD>(std::size(name))) != 0) {
            out.emplace_back(name);
        }
    }
    return out;
}

} // namespace krkrspeed
