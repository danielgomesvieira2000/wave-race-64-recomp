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

#include <algorithm>
#include <csignal>
#include <exception>
#include <stdexcept>
#include <atomic>
#include <chrono>
#include <mutex>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <psapi.h>
#include <dbghelp.h>

namespace {

// Once per process, shared by every site that resolves an address. Kept in one
// place because SymInitialize fails when the handle is already initialised, so a
// second site with its own flag concludes it has no symbols and prints bare
// addresses for the rest of the run.
bool ensure_symbols() {
    static const bool ready = [] {
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
        return SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
    }();
    return ready;
}

// Defined below, and used by the handler above it.
void describe_one_address(const char* label, void* address);

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
    const bool symbols_ready = ensure_symbols();

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

    // Which thread, and how it got there. Without these the report is an
    // address and a guess: a fault at an address in no loaded module means
    // something jumped through a pointer into freed memory, and the only
    // thing that says whose pointer it was is the stack underneath it.
    std::fprintf(stderr, "[wr64] on thread %lu", GetCurrentThreadId());
    PWSTR description = nullptr;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &description)) &&
        description != nullptr) {
        char name[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, description, -1, name,
                            static_cast<int>(sizeof(name)) - 1, nullptr, nullptr);
        if (name[0] != 0) {
            std::fprintf(stderr, " (%s)", name);
        }
        LocalFree(description);
    }
    std::fprintf(stderr, "\n");

    std::fprintf(stderr, "[wr64] stack:\n");
    void* frames[48] = {};
    const USHORT captured = CaptureStackBackTrace(0, 48, frames, nullptr);
    for (USHORT i = 0; i < captured; ++i) {
        describe_one_address("  ", frames[i]);
    }

    // That walk ends at the faulting frame and can go no further: unwinding
    // needs unwind data, and a jump into freed memory has none, so the caller --
    // the only thing that says whose dangling pointer this was -- is invisible to
    // it. It is not gone, though. A call pushes its return address, so it is
    // sitting at the top of the stack, with the frames above it intact. Scanning
    // for values that point into a loaded module recovers the path in.
    if (info->ContextRecord != nullptr) {
        const CONTEXT* context = info->ContextRecord;
        std::fprintf(stderr, "[wr64] rip %p, rsp %p\n",
                     reinterpret_cast<void*>(static_cast<uintptr_t>(context->Rip)),
                     reinterpret_cast<void*>(static_cast<uintptr_t>(context->Rsp)));

        const uintptr_t* stack = reinterpret_cast<const uintptr_t*>(context->Rsp);
        MEMORY_BASIC_INFORMATION region{};
        const bool readable =
            VirtualQuery(stack, &region, sizeof(region)) == sizeof(region) &&
            region.State == MEM_COMMIT &&
            (region.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                               PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_WRITECOPY)) != 0;
        if (readable) {
            const uintptr_t end =
                reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
            std::fprintf(stderr, "[wr64] return addresses still on the stack:\n");
            int shown = 0;
            for (int i = 0; i < 512 && shown < 12; ++i) {
                const uintptr_t slot = reinterpret_cast<uintptr_t>(stack + i);
                if (slot + sizeof(uintptr_t) > end) {
                    break;
                }
                const uintptr_t value = stack[i];
                if (value < 0x10000) {
                    continue;
                }
                HMODULE owner = nullptr;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCSTR>(value), &owner) &&
                    owner != nullptr) {
                    describe_one_address("  ", reinterpret_cast<void*>(value));
                    ++shown;
                }
            }
        }
    }

    std::fprintf(stderr, "[wr64] ================\n");
    std::fflush(stderr);

    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

// Shared by the crash report, the lookup-miss report and the hang watchdog.
static void describe_address(const char* label, void* address) {
    HANDLE process = GetCurrentProcess();
    const bool symbols_ready = ensure_symbols();

    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    // The module first, always. Most frames in a shutdown crash are in code this
    // project did not write and has no symbols for, and a bare "(no symbol)" does
    // not distinguish a graphics driver from SDL from freed memory -- which is
    // the distinction the whole report exists to make.
    char module_name[MAX_PATH] = {};
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module) && module != nullptr) {
        char path[MAX_PATH] = {};
        if (GetModuleFileNameA(module, path, sizeof(path)) != 0) {
            const char* slash = std::strrchr(path, '\\');
            std::snprintf(module_name, sizeof(module_name), "%s+0x%llX",
                          slash != nullptr ? slash + 1 : path,
                          static_cast<unsigned long long>(
                              reinterpret_cast<uintptr_t>(address) -
                              reinterpret_cast<uintptr_t>(module)));
        }
    }
    else {
        std::snprintf(module_name, sizeof(module_name), "no module -- freed or generated code");
    }

    DWORD64 displacement = 0;
    if (symbols_ready &&
        SymFromAddr(process, reinterpret_cast<DWORD64>(address), &displacement, symbol)) {
        std::fprintf(stderr, "[wr64] %s %s + 0x%llX  [%s]\n", label, symbol->Name,
                     static_cast<unsigned long long>(displacement), module_name);
    } else {
        std::fprintf(stderr, "[wr64] %s %p  [%s]\n", label, address, module_name);
    }

    IMAGEHLP_LINE64 line = {};
    line.SizeOfStruct = sizeof(line);
    DWORD line_displacement = 0;
    if (symbols_ready && SymGetLineFromAddr64(process, reinterpret_cast<DWORD64>(address),
                                              &line_displacement, &line)) {
        std::fprintf(stderr, "[wr64]     at %s:%lu\n", line.FileName, line.LineNumber);
    }
}

namespace {
void describe_one_address(const char* label, void* address) {
    describe_address(label, address);
}
}  // namespace

namespace wr64 {

void describe_code_address(const char* label, void* address) {
    describe_address(label, address);
}

}  // namespace wr64

namespace {

// Hang watchdog state. Only one piece of work is watched at a time, which is
// all this is for: catching a microcode that never returns.
//
// One persistent thread, armed with a deadline, rather than a thread per
// watched call. The first version of this spawned a detached std::thread for
// every call and let it poll for up to 100 ms before noticing the work had
// finished. Watching the audio microcode meant doing that for every audio
// task -- four per frame, some 240 thread creations a second, on the very
// thread that has to keep pace with the game. That jitter was enough to let
// the game get two audio frames ahead of the RSP and start rewriting a
// command list the microcode was still reading (see the ENVMIXER notes in
// docs/findings/phase-05.md). Arming a watchdog must cost two atomic stores.
std::atomic<long long> g_watch_deadline_ms{ 0 };   // 0 = nothing being watched
std::atomic<const char*> g_watch_what{ nullptr };
std::atomic<int> g_watch_seconds{ 0 };
HANDLE g_watch_target = nullptr;                  // the one thread that is watched
std::once_flag g_watch_thread_started;

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

void report_hang(HANDLE thread, const char* what, int seconds) {
    std::fprintf(stderr, "\n[wr64] ==== HANG ====\n");
    std::fprintf(stderr, "[wr64] %s has not returned after %d seconds\n", what, seconds);

    // One sample names a single instruction, which for a spin loop is whichever
    // one the thread happened to be on -- and with everything inlined that is
    // usually a helper, not the loop. Sampling repeatedly and reporting the
    // distinct addresses sketches the whole loop body instead.
    std::vector<void*> seen;
    for (int sample = 0; sample < 40; ++sample) {
        if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
            break;
        }
        CONTEXT context = {};
        context.ContextFlags = CONTEXT_CONTROL;
        void* rip = nullptr;
        if (GetThreadContext(thread, &context)) {
            rip = reinterpret_cast<void*>(context.Rip);
        }
        ResumeThread(thread);

        if (rip != nullptr && std::find(seen.begin(), seen.end(), rip) == seen.end()) {
            seen.push_back(rip);
        }
        Sleep(1);
    }
    for (void* rip : seen) {
        describe_address("in", rip);
    }

    std::fprintf(stderr, "[wr64] ==============\n");
    std::fflush(stderr);
}

void watch_thread() {
    for (;;) {
        Sleep(100);
        const long long deadline = g_watch_deadline_ms.load();
        if (deadline == 0 || now_ms() < deadline) {
            continue;
        }
        // Report once per hang: disarm before the (slow) sampling so a hang
        // is not reported again on the next tick.
        g_watch_deadline_ms.store(0);
        const char* what = g_watch_what.load();
        report_hang(g_watch_target, what != nullptr ? what : "watched work", g_watch_seconds.load());
    }
}

}  // namespace

namespace wr64 {

void watch_for_hang(const char* what, int seconds) {
    // The watched thread's handle is taken once: this only ever watches the
    // RSP task thread, and a per-call DuplicateHandle would be the kind of
    // cost this exists to avoid.
    static thread_local bool registered = false;
    if (!registered) {
        HANDLE duplicate = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                             GetCurrentProcess(), &duplicate, 0, FALSE,
                             DUPLICATE_SAME_ACCESS)) {
            return;
        }
        g_watch_target = duplicate;
        registered = true;
    }
    g_watch_what.store(what);
    g_watch_seconds.store(seconds);
    g_watch_deadline_ms.store(now_ms() + static_cast<long long>(seconds) * 1000);
    std::call_once(g_watch_thread_started, [] { std::thread{ watch_thread }.detach(); });
}

void watch_done() {
    g_watch_deadline_ms.store(0);
}

}  // namespace wr64

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

    // A C++ exception nobody catches ends in abort(), and abort() prints
    // "abort() has been called" and nothing else -- not the type, not the
    // message, not where. The vectored handler above does not help: it
    // deliberately ignores C++ exceptions, because they pass through it in
    // normal operation. Rethrowing inside terminate is the one way to get at
    // the exception that is actually killing the process.
    // abort() does not raise a structured exception, so the vectored handler
    // never sees it and the only trace it leaves is the CRT's four-word
    // "abort() has been called". A library that asserts its way out of a bad
    // state -- RmlUi and plume both do -- therefore kills the process with no
    // indication of which one, or where. Walking the stack here names it.
    std::signal(SIGABRT, [](int) {
        std::fprintf(stderr, "\n[wr64] ==== ABORT ====\n");
        void* frames[32] = {};
        const USHORT captured = CaptureStackBackTrace(0, 32, frames, nullptr);
        for (USHORT i = 0; i < captured; ++i) {
            describe_address("  ", frames[i]);
        }
        std::fprintf(stderr, "[wr64] ===============\n");
        std::fflush(stderr);
        _exit(3);
    });

    std::set_terminate([]() {
        std::fprintf(stderr, "\n[wr64] ==== UNHANDLED EXCEPTION ====\n");
        if (std::current_exception()) {
            try {
                std::rethrow_exception(std::current_exception());
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[wr64] %s\n", e.what());
            } catch (...) {
                std::fprintf(stderr, "[wr64] a non-std exception\n");
            }
        } else {
            std::fprintf(stderr, "[wr64] terminate called with no active exception\n");
        }
        std::fprintf(stderr, "[wr64] =============================\n");
        std::fflush(stderr);
        std::abort();
    });
}

}  // namespace wr64

#else

// Off Windows there is no dbghelp, and nothing here has an equivalent worth
// writing yet: a fault produces a core dump that a debugger reads properly,
// which is more than the Windows path can offer. What the rest of the port
// needs from this file is the *symbols* -- every one of these is called
// unconditionally from code that is not itself platform-gated, so a missing
// definition is a link error rather than a missing feature.
//
// The two that still say something useful say it. describe_code_address prints
// the raw address, which is what the Windows version prints when the symbol
// lookup fails anyway, and wr64_report_lookup_miss reports the recompiled
// function that could not be found -- see tools/patch_librecomp.py, which
// injects the call. The hang watchdog is the one real loss; it exists to catch
// recompiled microcode that spins forever, and rebuilding it on POSIX timers
// is work for the day that actually happens on Linux or macOS.

#include <cstdio>

namespace wr64 {

void install_crash_handler() {}

void describe_code_address(const char* label, void* address) {
    std::fprintf(stderr, "[wr64] %s: %p
", label, address);
}

void watch_for_hang(const char*, int) {}
void watch_done() {}

}  // namespace wr64

extern "C" void wr64_report_lookup_miss(unsigned int addr, void* return_address) {
    std::fprintf(stderr, "[wr64] function lookup failed at 0x%08X (caller %p)
",
                 addr, return_address);
    std::fflush(stderr);
}

#endif
