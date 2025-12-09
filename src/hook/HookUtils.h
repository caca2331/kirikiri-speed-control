#pragma once

#include <Windows.h>
#include <cstddef>

namespace krkrspeed {

// Patch the IAT entry in the main module for the given import.
bool PatchImport(const char *importModule, const char *functionName, void *replacement, void **original);

// Replace a vtable slot with a new function and return the old value.
template <typename T>
bool PatchVtableEntry(void **vtable, std::size_t index, T replacement, T &original) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    original = reinterpret_cast<T>(vtable[index]);
    vtable[index] = reinterpret_cast<void *>(replacement);
    VirtualProtect(&vtable[index], sizeof(void *), oldProtect, &oldProtect);
    return true;
}

} // namespace krkrspeed
