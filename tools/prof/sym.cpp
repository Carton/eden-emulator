// (local-only) PDB symbolizer for eden crash triage (WER / minidump RVAs).
//
// Build (VS dev prompt, from this directory):
//   cl /O2 /EHsc sym.cpp
// Usage:
//   sym.exe <eden.exe path> <hex RVA without 0x>
// Example (WER event "出错偏移: 0x0000000000ebab1c" on nvoglv64-hosting eden):
//   sym.exe build-vs22\bin\eden.exe ebab1c
// Notes:
//   - Base is assumed 0x140000000 (eden's default image base).
//   - RelWithDebInfo PDB sits next to the exe; /OPT:ICF folds identical
//     function bodies, so a hit may show a folded sibling lambda -- treat the
//     symbol as a cluster, not the literal frame (PROFILE §28.15).
//   - For non-eden modules (nvoglv64.dll etc.) pass that module's own path --
//     symbols are usually unavailable, but the RVA still tells whether two
//     crashes share a site (same offset == deterministic driver-side fault).
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#pragma comment(lib, "dbghelp.lib")
int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: sym.exe <exe> <hex-rva>\n"); return 2; }
    const char* exe = argv[1];
    DWORD64 rva = strtoull(argv[2], nullptr, 16);
    HANDLE p = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEBUG);
    SymInitialize(p, nullptr, FALSE);
    DWORD64 base = SymLoadModuleEx(p, nullptr, exe, nullptr, 0x140000000, 0, nullptr, 0);
    if (!base) { printf("load failed %lu\n", GetLastError()); return 1; }
    DWORD64 addr = 0x140000000 + rva;
    char buf[sizeof(SYMBOL_INFO) + 256] = {};
    auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (SymFromAddr(p, addr, &disp, si)) {
        printf("%s + %llu (0x%llx)\n", si->Name, (unsigned long long)disp, addr);
    } else { printf("SymFromAddr failed %lu\n", GetLastError()); }
    IMAGEHLP_LINE64 line = { sizeof(line) };
    DWORD ld = 0;
    if (SymGetLineFromAddr64(p, addr, &ld, &line)) {
        printf("%s:%lu\n", line.FileName, line.LineNumber);
    } else { printf("no line %lu\n", GetLastError()); }
    return 0;
}
