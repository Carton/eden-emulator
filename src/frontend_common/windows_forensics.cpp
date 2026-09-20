// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 1 only: no detours, external helper, watchdog or Vulkan probes.
// _exit/_Exit, quick_exit, ExitProcess, external/self TerminateProcess, inline
// fastfail and later replacement of handlers can bypass these hooks. VEH/UEF
// do NOT catch native fastfail. In-process DbgHelp can deadlock even preloaded.
// Thread-local invalid-parameter handlers and separate CRTs are not covered.
// Stack guarantee applies only to main; worker stack exhaustion remains best effort.
// Emergency writes use fixed buffers, no allocator, STL formatting or logger.
#include "frontend_common/windows_forensics.h"

#if defined(_WIN32) && defined(_MSC_VER)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <signal.h>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <process.h>
#include <stdint.h>
#include <strsafe.h>
#pragma comment(lib, "dbghelp.lib")

namespace FrontendCommon {
namespace {
HANDLE emergency = INVALID_HANDLE_VALUE;
HANDLE context_file = INVALID_HANDLE_VALUE;
HANDLE dump_file = INVALID_HANDLE_VALUE;
volatile LONG fatal_active = 0;
volatile LONG veh_active = 0;
volatile LONG event_active = 0;
LPTOP_LEVEL_EXCEPTION_FILTER old_filter = nullptr;
std::terminate_handler old_terminate = nullptr;
std::new_handler old_new = nullptr;
_invalid_parameter_handler old_invalid = nullptr;
using SignalHandler = void(__cdecl*)(int);
SignalHandler old_abort = SIG_DFL;
using DumpFunction = decltype(&MiniDumpWriteDump);
DumpFunction write_dump = nullptr;

struct Line {
    char data[2048];
    DWORD size = 0;
    void Text(const char* s) noexcept {
        while (*s && size < sizeof(data) - 1) data[size++] = *s++;
    }
    void Hex(ULONG64 n) noexcept {
        Text("0x");
        for (int shift = 60; shift >= 0; shift -= 4) {
            if (size < sizeof(data) - 1) data[size++] = "0123456789abcdef"[(n >> shift) & 15];
        }
    }
};

void Write(HANDLE h, const void* data, DWORD size) noexcept {
    if (!h || h == INVALID_HANDLE_VALUE) return;
    auto* bytes = static_cast<const char*>(data);
    while (size) {
        DWORD done = 0;
        if (!WriteFile(h, bytes, size, &done, nullptr) || !done) break;
        bytes += done;
        size -= done;
    }
    FlushFileBuffers(h);
}

void Event(const char* tag, ULONG64 code = 0, const void* pc = nullptr) noexcept {
    // Never wait on a logging lock in a fault handler (including recursive I/O faults).
    if (InterlockedCompareExchange(&event_active, 1, 0)) return;
    Line line;
    LARGE_INTEGER qpc;
    FILETIME utc;
    QueryPerformanceCounter(&qpc);
    GetSystemTimeAsFileTime(&utc);
    line.Text(tag);
    line.Text(" tid="); line.Hex(GetCurrentThreadId());
    line.Text(" utc_filetime="); line.Hex((ULONG64{utc.dwHighDateTime} << 32) | utc.dwLowDateTime);
    line.Text(" qpc="); line.Hex(static_cast<ULONG64>(qpc.QuadPart));
    line.Text(" code="); line.Hex(code);
    line.Text(" pc="); line.Hex(reinterpret_cast<ULONG64>(pc));
    line.Text("\r\n");
    Write(emergency, line.data, line.size);
    // Avoid a blocked stderr pipe in an emergency. The file is authoritative.
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (GetFileType(err) == FILE_TYPE_DISK) Write(err, line.data, line.size);
    InterlockedExchange(&event_active, 0);
}

void Stack() noexcept {
    void* pcs[32]{};
    const USHORT count = RtlCaptureStackBackTrace(0, 32, pcs, nullptr);
    Line line;
    line.Text("STACK raw_pc=");
    for (USHORT i = 0; i < count; ++i) {
        line.Hex(reinterpret_cast<ULONG64>(pcs[i])); line.Text(" ");
    }
    line.Text("\r\n");
    Write(emergency, line.data, line.size);
}

void Capture(const char* tag, EXCEPTION_POINTERS* exception = nullptr) noexcept {
    Event(tag, exception ? exception->ExceptionRecord->ExceptionCode : 0,
          exception ? exception->ExceptionRecord->ExceptionAddress : nullptr);
    if (InterlockedCompareExchange(&fatal_active, 1, 0)) {
        Event("CAPTURE_ALREADY_CLAIMED");
        return;
    }
    // Binary layout: native Windows CONTEXT, followed by EXCEPTION_RECORD.
    // Pointer fields in the record are diagnostic addresses, not owned pointers.
    CONTEXT context{};
    EXCEPTION_RECORD record{};
    if (exception) {
        context = *exception->ContextRecord;
        record = *exception->ExceptionRecord;
    } else {
        RtlCaptureContext(&context);
        record.ExceptionCode = 0xE000ED01; // explicitly synthetic hook context
#if defined(_M_X64)
        record.ExceptionAddress = reinterpret_cast<void*>(context.Rip);
#elif defined(_M_ARM64)
        record.ExceptionAddress = reinterpret_cast<void*>(context.Pc);
#endif
    }
    Write(context_file, &context, sizeof(context));
    Write(context_file, &record, sizeof(record));
    Stack();
    EXCEPTION_POINTERS pointers{&record, &context};
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), &pointers, FALSE};
    Event("DUMP_BEGIN");
    if (write_dump && dump_file != INVALID_HANDLE_VALUE) {
        const BOOL ok = write_dump(GetCurrentProcess(), GetCurrentProcessId(), dump_file,
            static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo),
            &info, nullptr, nullptr);
        const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        FlushFileBuffers(dump_file);
        Event(ok ? "DUMP_DONE" : "DUMP_FAILED", error);
    } else {
        Event("DUMP_UNAVAILABLE");
    }
    // Keep the first context/dump intact (terminate -> abort can enter twice).
}

LONG WINAPI Unhandled(EXCEPTION_POINTERS* p) {
    Capture("UNHANDLED_EXCEPTION", p);
    return old_filter ? old_filter(p) : EXCEPTION_CONTINUE_SEARCH;
}
LONG WINAPI Vectored(EXCEPTION_POINTERS* p) {
    const DWORD code = p->ExceptionRecord->ExceptionCode;
    if ((code == 0xC0000005 || code == 0xC00000FD || code == 0xC0000409) &&
        !InterlockedCompareExchange(&veh_active, 1, 0)) {
        Event("VEH_FIRST_CHANCE", code, p->ExceptionRecord->ExceptionAddress);
        InterlockedExchange(&veh_active, 0);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
void Terminate() noexcept {
    Capture("STD_TERMINATE");
    if (old_terminate) old_terminate();
    std::abort();
}
void __cdecl Invalid(const wchar_t* expression, const wchar_t* function,
                    const wchar_t* file, unsigned int line, uintptr_t reserved) {
    Capture("INVALID_PARAMETER");
    if (old_invalid) {
        old_invalid(expression, function, file, line, reserved);
        return;
    }
    _invoke_watson(expression, function, file, line, reserved);
}
void NewFailure() {
    Capture("NEW_FAILURE");
    if (old_new) { old_new(); return; }
    throw std::bad_alloc{};
}
void __cdecl Abort(int sig) {
    Capture("SIGABRT");
    if (old_abort != SIG_DFL && old_abort != SIG_IGN && old_abort != SIG_ERR) old_abort(sig);
    // Returning to abort preserves its original CRT termination/report behavior.
}
void AtExit() noexcept {
    Event("EXIT_BEGIN");
    const auto result = Common::Log::ForensicsFlushLogging();
    if (result != 0) Event(result == 1 ? "EXIT_FLUSH_TIMEOUT" : "EXIT_FLUSH_FAILED", result);
    Event("EXIT_FLUSH_DONE", result);
}

bool MakeDirectory(const wchar_t* path) noexcept {
    return CreateDirectoryW(path, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}
bool OpenSession(const wchar_t* root) noexcept {
    wchar_t path[MAX_PATH];
    SYSTEMTIME utc;
    GetSystemTime(&utc);
    if (!MakeDirectory(root) || FAILED(StringCchPrintfW(path, MAX_PATH, L"%s\\dumps", root)) ||
        !MakeDirectory(path)) return false;
    if (FAILED(StringCchPrintfW(path, MAX_PATH,
        L"%s\\dumps\\%04u%02u%02uT%02u%02u%02u%03uZ-%lu", root,
        utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond,
        utc.wMilliseconds, GetCurrentProcessId())) || !MakeDirectory(path)) return false;
    wchar_t name[MAX_PATH];
    auto open = [&](const wchar_t* leaf) {
        if (FAILED(StringCchPrintfW(name, MAX_PATH, L"%s\\%s", path, leaf))) return INVALID_HANDLE_VALUE;
        return CreateFileW(name, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    };
    emergency = open(L"emergency.log");
    if (emergency == INVALID_HANDLE_VALUE) return false;
    context_file = open(L"context.bin");
    dump_file = open(L"crash.dmp");
    return true;
}
DWORD WINAPI KillSelf(void*) {
    TerminateProcess(GetCurrentProcess(), 6);
    return 0;
}
__declspec(noinline) void SelfTest(unsigned mode) {
    // Suppress unattended WER UI only in explicitly requested suicide tests.
    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    Event("SELFTEST_BEGIN", mode);
    switch (mode) {
    case 1: {
        // Volatile address prevents compile-time folding of the intentional AV.
        volatile uintptr_t address = 0;
        *reinterpret_cast<volatile int*>(address) = 1;
        break;
    }
    case 2: std::abort();
    case 3: std::terminate();
    case 4: {
        // Retail UCRT exports only the noinfo entry: it dispatches exactly
        // _invalid_parameter(nullptr, nullptr, nullptr, 0, 0). The named
        // five-argument entry is debug-only (including its import library).
        _invalid_parameter_noinfo();
        break;
    }
    case 5: _exit(5);
    case 6: {
        HANDLE thread = CreateThread(nullptr, 0, KillSelf, nullptr, 0, nullptr);
        if (thread) { WaitForSingleObject(thread, INFINITE); CloseHandle(thread); }
        break;
    }
    }
    Event("SELFTEST_UNEXPECTED_RETURN", GetLastError());
    _exit(99);
}
} // namespace

bool InitWindowForensics() noexcept {
    const char* value = std::getenv("EDEN_FORENSICS");
    if (!value) return false;
    unsigned test = 0;
    if (std::strcmp(value, "1") != 0) {
        if (std::strncmp(value, "selftest=", 9) != 0 || value[9] < '1' || value[9] > '6' || value[10])
            return false;
        test = static_cast<unsigned>(value[9] - '0');
    }
    wchar_t root[MAX_PATH];
    const DWORD size = GetEnvironmentVariableW(L"EDEN_PROF_DATA", root, MAX_PATH);
    if (!size) StringCchCopyW(root, MAX_PATH, L"F:\\prof");
    if (size >= MAX_PATH || !OpenSession(root)) {
        const DWORD temp_size = GetTempPathW(MAX_PATH, root);
        if (!temp_size || temp_size >= MAX_PATH || !OpenSession(root)) {
            OutputDebugStringW(L"EDEN_FORENSICS: cannot open emergency session\n");
            if (test) _exit(98);
            return false;
        }
    }
    LoadLibraryExW(L"dbgcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dbghelp) write_dump = reinterpret_cast<DumpFunction>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    old_terminate = std::set_terminate(Terminate);
    old_invalid = _set_invalid_parameter_handler(Invalid);
    old_new = std::set_new_handler(NewFailure);
    old_abort = signal(SIGABRT, Abort);
    ULONG guarantee = 64 * 1024;
    Event(SetThreadStackGuarantee(&guarantee) ? "MAIN_STACK_GUARANTEE" : "STACK_GUARANTEE_FAILED");
    Event(AddVectoredExceptionHandler(1, Vectored) ? "VEH_READY" : "VEH_FAILED");
    old_filter = SetUnhandledExceptionFilter(Unhandled);
    Event("IMAGE_BASE", reinterpret_cast<ULONG64>(GetModuleHandleW(nullptr)));
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    Event("QPC_FREQUENCY", static_cast<ULONG64>(frequency.QuadPart));
    Event(std::atexit(AtExit) == 0 ? "ATEXIT_READY" : "ATEXIT_FAILED");
    Event("FORENSICS_READY", write_dump != nullptr);
    if (test) SelfTest(test);
    return true;
}
void ForensicsMainReturn() noexcept { Event("MAIN_RETURN"); }
} // namespace FrontendCommon
#else
namespace FrontendCommon {
bool InitWindowForensics() noexcept { return false; }
void ForensicsMainReturn() noexcept {}
}
#endif
