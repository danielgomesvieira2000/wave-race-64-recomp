# patches

The port's C++ that stands in for, wraps or reaches into game functions. Every
`.cpp` here is compiled into the executable (`CMakeLists.txt` globs them), in
three ways:

| How | Example | Where it is wired |
|---|---|---|
| **Replaces** a recompiled function: a strong definition wins over the generated weak `RECOMP_FUNC` at link time | `dma.cpp` | the function is listed under `ignored` in `recomp/wr64.toml`, so nothing is generated for it |
| **Wraps** one, registered at the game function's address so calls reach the wrapper first | `framerate.cpp`, `water.cpp` | `src/overlays.cpp` |
| **Is called from inside** one, by a `[[patches.hook]]` pasted into the generated C | `buoys.cpp` | `recomp/wr64.toml`; changing a hook means regenerating the game sources (`docs/BUILDING.md`) |

`ui_funcs.h` is included by recompui through a fixed path.
