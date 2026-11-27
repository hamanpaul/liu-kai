#include <windows.h>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>

// Test helper: load a supplied DLL without registering the input method.
// A bounded lifetime prevents the test from leaving a persistent process.
int wmain(int argc,wchar_t** argv) {
    if (argc < 2 || argc > 4) return 2;
    unsigned long hold_seconds=45;
    if (argc == 4) {
        wchar_t* end=nullptr;
        hold_seconds=std::wcstoul(argv[3],&end,10);
        if (!end || *end || hold_seconds == 0 ||
            hold_seconds > 600 ||
            hold_seconds == std::numeric_limits<unsigned long>::max()) return 2;
    }
    HMODULE module=LoadLibraryW(argv[1]);
    if (!module) { std::cerr << "LoadLibrary failed: " << GetLastError() << '\n'; return 1; }
    std::cout << "ready pid=" << GetCurrentProcessId() << std::endl;
    if (argc >= 3) {
        std::ofstream ready{std::filesystem::path(argv[2])};
        if (!ready || !(ready << "ready pid=" << GetCurrentProcessId() << '\n')) { FreeLibrary(module); return 3; }
    }
    Sleep(static_cast<DWORD>(hold_seconds)*1000);
    FreeLibrary(module);
    return 0;
}
