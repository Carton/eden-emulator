// (local-only) Resolve a module-relative hexadecimal RVA using its adjacent PDB.
// Build: cl /W4 /EHsc /std:c++20 sym.cpp
// Usage: sym.exe <module path> <hex RVA>
// /OPT:ICF may fold different functions to the same address.
#include <windows.h>
#include <dbghelp.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <limits>

#pragma comment(lib, "dbghelp.lib")

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: sym.exe <module> <hex-rva>\n");
        return 2;
    }
    char* end{};
    errno = 0;
    const DWORD64 rva = std::strtoull(argv[2], &end, 16);
    if (errno || end == argv[2] || *end || argv[2][0] == '-') {
        std::fprintf(stderr, "invalid hexadecimal RVA\n");
        return 2;
    }
    const HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
    if (!SymInitialize(process, nullptr, FALSE)) {
        std::fprintf(stderr, "SymInitialize failed %lu\n", GetLastError());
        return 1;
    }
    struct Cleanup {
        HANDLE process;
        ~Cleanup() { SymCleanup(process); }
    } cleanup{process};
    const DWORD64 base = SymLoadModuleEx(process, nullptr, argv[1], nullptr,
                                        0x140000000ULL, 0, nullptr, 0);
    if (!base || rva > (std::numeric_limits<DWORD64>::max)() - base) {
        std::fprintf(stderr, "module load or address overflow error %lu\n", GetLastError());
        return 1;
    }
    IMAGEHLP_MODULE64 module{};
    module.SizeOfStruct = sizeof(module);
    if (!SymGetModuleInfo64(process, base, &module) || rva >= module.ImageSize) {
        std::fprintf(stderr, "RVA is outside the loaded module\n");
        return 2;
    }
    const DWORD64 address = base + rva;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement{};
    if (!SymFromAddr(process, address, &displacement, symbol)) {
        std::fprintf(stderr, "SymFromAddr failed %lu\n", GetLastError());
        return 1;
    }
    std::printf("%s + %llu (0x%llx)\n", symbol->Name,
                static_cast<unsigned long long>(displacement),
                static_cast<unsigned long long>(address));
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD line_displacement{};
    if (SymGetLineFromAddr64(process, address, &line_displacement, &line)) {
        std::printf("%s:%lu\n", line.FileName, line.LineNumber);
    } else {
        std::printf("no source line (%lu)\n", GetLastError());
    }
    return 0;
}
