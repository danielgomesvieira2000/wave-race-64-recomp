#ifndef WR64_UI_FUNCS_H
#define WR64_UI_FUNCS_H

// recompui includes this file by a hardcoded relative path from inside the
// library:
//
//     #include "../../../../../patches/ui_funcs.h"
//
// which resolves to this file in the project root. Upstream marks it "TODO:
// Forced game includes" -- the library is written expecting to sit inside a
// port that provides a header of the game-side declarations recompui's mod API
// calls into. There is no way to satisfy it other than by having the file, so
// the project has it.
//
// What recompui actually needs from here today is the event structures, which
// live in the library itself; the indirection exists so a port can add its own
// mod-facing declarations alongside them. Mods load, but this port exports no
// API of its own to them, so the file is exactly that include and nothing more.
// If it ever does, the declarations belong here.

#include "recompui/event_structs.h"

#endif  // WR64_UI_FUNCS_H
