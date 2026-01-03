#include "HookUtils.h"

#include <cstring>
#include <cstdint>

namespace krkrspeed {

namespace {
bool patchThunk(void *address, void *replacement, void **original) {
    if (!address || !replacement) return false;
    DWORD oldProtect = 0;
    if (!VirtualProtect(address, sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    if (original && !*original) {
        *original = reinterpret_cast<void *>(*reinterpret_cast<ULONG_PTR *>(address));
    }
    *reinterpret_cast<ULONG_PTR *>(address) = reinterpret_cast<ULONG_PTR>(replacement);
    VirtualProtect(address, sizeof(void *), oldProtect, &oldProtect);
    return true;
}
} // namespace

bool PatchImportInModule(HMODULE module, const char *importModule, const char *functionName, void *replacement,
                         void **original) {
    if (!module || !importModule || !functionName || !replacement) {
        return false;
    }
    auto *dos = reinterpret_cast<PIMAGE_DOS_HEADER>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto *nt = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<std::uint8_t *>(module) + dos->e_lfanew);
    const auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) return false;

    auto *imports = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(
        reinterpret_cast<std::uint8_t *>(module) + dir.VirtualAddress);

    for (; imports && imports->Name; ++imports) {
        const char *dllName =
            reinterpret_cast<const char *>(reinterpret_cast<std::uint8_t *>(module) + imports->Name);
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

            return patchThunk(&thunk->u1.Function, replacement, original);
        }
    }
    return false;
}

bool PatchImport(const char *importModule, const char *functionName, void *replacement, void **original) {
    HMODULE module = GetModuleHandleW(nullptr);
    if (!module) return false;
    return PatchImportInModule(module, importModule, functionName, replacement, original);
}

} // namespace krkrspeed
