#include <Windows.h>
#include <string>
#include <vector>
#include <iostream>
#include <filesystem>

namespace {

int wmain_impl(int argc, wchar_t **argv) {
    if (argc < 3) {
        std::wcerr << L"Usage: krkr_injector.exe <pid> <dll_path>\n";
        return 2;
    }
    DWORD pid = std::wcstoul(argv[1], nullptr, 10);
    std::wstring dllPath = argv[2];
    if (pid == 0 || dllPath.empty()) {
        std::wcerr << L"Invalid arguments\n";
        return 2;
    }

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                     PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) {
        std::wcerr << L"OpenProcess failed: " << GetLastError() << L"\n";
        return 3;
    }

    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remoteMemory = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMemory) {
        std::wcerr << L"VirtualAllocEx failed: " << GetLastError() << L"\n";
        CloseHandle(process);
        return 4;
    }

    if (!WriteProcessMemory(process, remoteMemory, dllPath.c_str(), bytes, nullptr)) {
        std::wcerr << L"WriteProcessMemory failed: " << GetLastError() << L"\n";
        VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
        CloseHandle(process);
        return 5;
    }

    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel, "LoadLibraryW"));
    if (!loadLibrary) {
        std::wcerr << L"GetProcAddress LoadLibraryW failed\n";
        VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
        CloseHandle(process);
        return 6;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remoteMemory, 0, nullptr);
    if (!thread) {
        std::wcerr << L"CreateRemoteThread failed: " << GetLastError() << L"\n";
        VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
        CloseHandle(process);
        return 7;
    }

    DWORD waitResult = WaitForSingleObject(thread, 5000);
    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);

    CloseHandle(thread);
    VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
    CloseHandle(process);

    if (waitResult == WAIT_TIMEOUT) return 8;
    if (waitResult == WAIT_FAILED) return 9;
    if (exitCode == 0) return 10;
    return 0;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    return wmain_impl(argc, argv);
}
