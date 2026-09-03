#pragma once

namespace wr64 {

// Installs a handler that reports the faulting address and owning module before
// the process dies. See src/crash_handler.cpp for why this is worth having.
void install_crash_handler();

// Prints where a code address is, resolved to a function name and source line.
// Shared with the crash report, which is where the symbol lookup already lives.
void describe_code_address(const char* label, void* address);

// Watches a piece of work that is expected to finish quickly, and if it has not
// finished after `seconds`, reports where the calling thread is stuck. A
// recompiled microcode that spins forever is otherwise invisible: no fault, no
// output, just a thread that never comes back and a game that stops.
void watch_for_hang(const char* what, int seconds);
void watch_done();

}  // namespace wr64
