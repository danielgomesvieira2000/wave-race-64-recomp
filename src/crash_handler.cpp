// Phase 04: report where a crash happened instead of losing it.
//
// A recompiled game that faults gives nothing useful by default: the process
// dies, and the only evidence is an exit code that tools routinely misreport.
// This session lost time to exactly that -- cmd expands %errorlevel% at parse
// time, so a segfault read back as a clean exit 0 and looked like an orderly
// shutdown.
//
// A vectored exception handler catches the fault before the process unwinds and
// prints the faulting address, the address being accessed, and the module the
// instruction belongs to. That last part is what matters most here: it says
// whether the fault is in the recompiled game code, in RT64, or in the runtime,
// which is the first question worth answering about any crash in this project.

#include "wr64/crash_handler.h"

#if defined(_WIN32)

#include <cstdio>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <psapi.h>
#include <dbghelp.h>

namespace {

const char* exception_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        default:                              return "unknown";
    }
}

LONG WINAPI on_exception(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    const DWORD code = record->ExceptionCode;

    // Only report faults that actually kill the process. Debug breakpoints and
    // C++ exceptions pass through here too and are not interesting.
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_IN_PAGE_ERROR) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void* pc = record->ExceptionAddress;

    std::fprintf(stderr, "\n[wr64] ==== CRASH ====\n");
    std::fprintf(stderr, "[wr64] %s (0x%08lX) at %p\n", exception_name(code),
                 static_cast<unsigned long>(code), pc);

    if (code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const char* how = record->ExceptionInformation[0] == 0 ? "reading"
                        : record->ExceptionInformation[0] == 1 ? "writing"
                        : "executing";
        std::fprintf(stderr, "[wr64] while %s address 0x%llX\n", how,
                     static_cast<unsigned long long>(record->ExceptionInformation[1]));
    }

    // Naming the module answers the question that matters: whose code faulted.
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(pc), &module) && module != nullptr) {
        char path[MAX_PATH] = {};
        if (GetModuleFileNameA(module, path, sizeof(path)) != 0) {
            std::fprintf(stderr, "[wr64] in module %s (base %p, offset 0x%llX)\n",
                         path, static_cast<void*>(module),
                         static_cast<unsigned long long>(
                             reinterpret_cast<uintptr_t>(pc) -
                             reinterpret_cast<uintptr_t>(module)));
        }
    }

    // The module alone is not enough: the recompiled game code is linked into
    // the executable, so "in WaveRace64Recomp.exe" covers ~1,300 game functions
    // as well as the harness. Resolving the symbol names what actually faulted,
    // which is the difference between a usable report and a hex address.
    HANDLE process = GetCurrentProcess();
    static bool symbols_ready = false;
    if (!symbols_ready) {
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
        symbols_ready = SymInitialize(process, nullptr, TRUE) != FALSE;
    }

    if (symbols_ready) {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
        SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;

        DWORD64 displacement = 0;
        if (SymFromAddr(process, reinterpret_cast<DWORD64>(pc), &displacement, symbol)) {
            std::fprintf(stderr, "[wr64] in function %s + 0x%llX\n", symbol->Name,
                         static_cast<unsigned long long>(displacement));
        }

        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(line);
        DWORD line_displacement = 0;
        if (SymGetLineFromAddr64(process, reinterpret_cast<DWORD64>(pc),
                                 &line_displacement, &line)) {
            std::fprintf(stderr, "[wr64] at %s:%lu\n", line.FileName, line.LineNumber);
        }
    }

    std::fprintf(stderr, "[wr64] ================\n");
    std::fflush(stderr);

    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

namespace {

// Shared by the crash report and the lookup-miss report.
void describe_address(const char* label, void* address) {
    HANDLE process = GetCurrentProcess();
    static bool symbols_ready = false;
    if (!symbols_ready) {
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
        symbols_ready = SymInitialize(process, nullptr, TRUE) != FALSE;
    }
    if (!symbols_ready) {
        std::fprintf(stderr, "[wr64] %s %p\n", label, address);
        return;
    }

    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    DWORD64 displacement = 0;
    if (SymFromAddr(process, reinterpret_cast<DWORD64>(address), &displacement, symbol)) {
        std::fprintf(stderr, "[wr64] %s %s + 0x%llX\n", label, symbol->Name,
                     static_cast<unsigned long long>(displacement));
    } else {
        std::fprintf(stderr, "[wr64] %s %p (no symbol)\n", label, address);
    }

    IMAGEHLP_LINE64 line = {};
    line.SizeOfStruct = sizeof(line);
    DWORD line_displacement = 0;
    if (SymGetLineFromAddr64(process, reinterpret_cast<DWORD64>(address),
                             &line_displacement, &line)) {
        std::fprintf(stderr, "[wr64]     at %s:%lu\n", line.FileName, line.LineNumber);
    }
}

}  // namespace

// Called by librecomp's get_function when an address lookup fails. See
// tools/patch_librecomp.py for why that call site exists.
//
// The address alone was never enough: it does not say which function asked, and
// with 49 threads running it does not say which thread. Both matter here,
// because the frame loop is spread across threads and reasoning about "the next
// iteration" from the disassembly has already proved unreliable.
extern "C" void wr64_report_lookup_miss(unsigned int addr, void* return_address) {
    static long misses = 0;
    const long index = InterlockedIncrement(&misses);

    std::fprintf(stderr, "\n[wr64] ==== LOOKUP MISS #%ld ====\n", index);
    std::fprintf(stderr, "[wr64] no function registered at 0x%08X\n", addr);
    std::fprintf(stderr, "[wr64] thread %lu\n", GetCurrentThreadId());
    describe_address("called from", return_address);
    std::fprintf(stderr, "[wr64] ==========================\n");
    std::fflush(stderr);
}

namespace wr64 {

void install_crash_handler() {
    AddVectoredExceptionHandler(1, on_exception);
}

}  // namespace wr64

#else

namespace wr64 {
void install_crash_handler() {}
}  // namespace wr64

#endif
