#include "HookUtils.h"

#include <cstring>
#include <cstdint>

namespace krkrspeed {

bool PatchImport(const char *importModule, const char *functionName, void *replacement, void **original) {
    HMODULE module = GetModuleHandleW(nullptr);
    if (!module) return false;

    ULONG size = 0;
    auto *dos = reinterpret_cast<PIMAGE_DOS_HEADER>(module);
    auto *nt = reinterpret_cast<PIMAGE_NT_HEADERS>((reinterpret_cast<std::uint8_t *>(module)) + dos->e_lfanew);
    const auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) return false;

    auto *imports = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(
        reinterpret_cast<std::uint8_t *>(module) + dir.VirtualAddress);

    for (; imports && imports->Name; ++imports) {
        const char *dllName = reinterpret_cast<const char *>(reinterpret_cast<std::uint8_t *>(module) + imports->Name);
        if (_stricmp(dllName, importModule) != 0) {
            continue;
        }

        auto *thunkOrig = reinterpret_cast<PIMAGE_THUNK_DATA>(
            reinterpret_cast<std::uint8_t *>(module) + imports->OriginalFirstThunk);
        auto *thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(
            reinterpret_cast<std::uint8_t *>(module) + imports->FirstThunk);

        for (; thunkOrig && thunkOrig->u1.AddressOfData; ++thunkOrig, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(thunkOrig->u1.Ordinal)) {
                continue;
            }
            auto *import = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(
                reinterpret_cast<std::uint8_t *>(module) + thunkOrig->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char *>(import->Name), functionName) != 0) {
                continue;
            }

            DWORD oldProtect = 0;
            if (!VirtualProtect(&thunk->u1.Function, sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                return false;
            }
            *original = reinterpret_cast<void *>(thunk->u1.Function);
            thunk->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            VirtualProtect(&thunk->u1.Function, sizeof(void *), oldProtect, &oldProtect);
            return true;
        }
    }
    return false;
}

} // namespace krkrspeed
