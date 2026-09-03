#pragma once

namespace wr64 {

// Installs a handler that reports the faulting address and owning module before
// the process dies. See src/crash_handler.cpp for why this is worth having.
void install_crash_handler();

}  // namespace wr64
