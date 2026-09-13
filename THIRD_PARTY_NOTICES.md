# Third-party notices

This project's own code (`src/`, `include/`, `patches/`, `tools/`, `recomp/`,
`assets/icons/`, `assets/recomp.rcss`) is under the MIT License in `LICENSE`.
A built executable also contains, or ships beside, the following, under their own
terms. **Every release carries these license texts in its `licenses/` folder**
(inside the app bundle's `Contents/Resources` on macOS); the list it is built
from is `tools/third_party_licenses.txt`, and the texts that exist only in a
source file's header are copied into `licenses/` in this repository.

Portions of this software are copyright © The FreeType Project
(www.freetype.org). All rights reserved.

### Runtime, renderer and frontend

| Component | Role | License | Text |
|---|---|---|---|
| [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) (librecomp, ultramodern) | the runtime the recompiled game runs on | **GPL-3.0** | `lib/N64ModernRuntime/COPYING` |
| [N64Recomp](https://github.com/N64Recomp/N64Recomp) (with LiveRecomp, SymbolLists), RSPRecomp | the recompiler; its runtime headers and the LiveRecomp libraries are linked in | MIT | `lib/N64ModernRuntime/N64Recomp/LICENSE` |
| [RT64](https://github.com/rt64/rt64) | the renderer | MIT | `lib/RT64/LICENSE` |
| [RecompFrontend](https://github.com/N64Recomp/RecompFrontend) (recompui, recompinput) | menus, controller mapping | **no license published** as of this release | -- (see *Open points*) |
| [plume](https://github.com/renderbag/plume) | RT64's graphics API layer | MIT | `lib/RT64/src/contrib/plume/LICENSE` |
| [RmlUi](https://github.com/mikke89/RmlUi), with its robin_hood and itlib containers | the UI toolkit recompui is built on | MIT | `lib/RecompFrontend/recompui/lib/RmlUi/LICENSE.txt`, `.../Include/RmlUi/Core/Containers/LICENSE.txt` |
| [FreeType](https://freetype.org) | RmlUi's font rasteriser (statically linked on Windows, system library on Linux, bundled on macOS) | FTL (FreeType License) | `lib/RecompFrontend/recompui/lib/freetype-windows-binaries/LICENSE.TXT`, `FTL.TXT` |
| [lunasvg](https://github.com/sammycage/lunasvg) and its [plutovg](https://github.com/sammycage/plutovg) | SVG icons in the menus | MIT; plutovg's FreeType-derived rasteriser under FTL | `lib/RecompFrontend/recompui/lib/lunasvg/LICENSE`, `.../plutovg/LICENSE`, `.../plutovg/source/FTL.TXT` |
| GamepadMotionHelpers | gyro handling in recompinput | MIT | `lib/RecompFrontend/lib/GamepadMotionHelpers/LICENSE` |
| SlotMap (Sergey Makeev) | recompui's element storage | MIT | `licenses/slotmap.txt` |
| [SDL2](https://libsdl.org) | window, input, audio (shipped as SDL2.dll on Windows, bundled on macOS, system library on Linux) | Zlib | `lib/RT64/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3/COPYING.txt` |
| [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) (dxcompiler.dll, dxil.dll) | shader compilation, Windows | Microsoft Software License Terms, with MIT and LLVM components | `licenses/dxc-microsoft.txt`, `licenses/dxc-mit.txt`, `licenses/dxc-llvm.txt` (see *Open points*) |

### Libraries compiled in

| Component | Role | License | Text |
|---|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui), [ImPlot](https://github.com/epezent/implot), [im3d](https://github.com/john-chapman/im3d) | RT64's debug menu (F1) | MIT | `lib/RT64/src/contrib/imgui/LICENSE.txt`, `.../implot/LICENSE`, `.../im3d/LICENSE` |
| [hlsl++](https://github.com/redorav/hlslpp) | RT64's shader-style math | MIT | `lib/RT64/src/contrib/hlslpp/LICENSE` |
| [nlohmann/json](https://github.com/nlohmann/json) | settings, texture packs, profiles | MIT | `licenses/nlohmann-json.txt` |
| [stb_image](https://github.com/nothings/stb) (and stb_truetype in plutovg) | texture pack images, SVG fonts | MIT or Unlicense | `lib/RT64/src/contrib/stb/LICENSE` |
| [ddspp](https://github.com/redorav/ddspp) | DDS textures in packs | MIT | `lib/RT64/src/contrib/ddspp/LICENSE` |
| [xxHash](https://github.com/Cyan4973/xxHash) | texture hashing | BSD-2-Clause | `lib/RT64/src/contrib/xxHash/LICENSE` |
| [zstd](https://github.com/facebook/zstd) | compression | BSD-3-Clause (dual GPL-2.0; used under BSD) | `lib/RT64/src/contrib/zstd/LICENSE` |
| [miniz](https://github.com/richgel999/miniz) | zip reading for mods (librecomp's copy, MIT) and RT64 (its copy, Unlicense) | MIT / Unlicense | `lib/N64ModernRuntime/thirdparty/miniz/LICENSE`, `licenses/miniz-rt64.txt` |
| [o1heap](https://github.com/pavel-kirienko/o1heap) | runtime heap | MIT | `lib/N64ModernRuntime/thirdparty/o1heap/LICENSE` |
| [moodycamel::ConcurrentQueue](https://github.com/cameron314/concurrentqueue) | runtime and UI queues | BSD-2-Clause (or Boost 1.0) | `licenses/concurrentqueue.txt` |
| [{fmt}](https://github.com/fmtlib/fmt), [rabbitizer](https://github.com/Decompollaborate/rabbitizer), [sljit](https://github.com/zherczeg/sljit) | LiveRecomp, for code mods | MIT, MIT, BSD-2-Clause | `lib/N64ModernRuntime/N64Recomp/lib/{fmt,rabbitizer,sljit}/LICENSE` |
| [nativefiledialog-extended](https://github.com/btzy/nativefiledialog-extended) | the file picker for the dump | Zlib | `lib/RT64/src/contrib/nativefiledialog-extended/LICENSE` |
| [re-spirv](https://github.com/renderbag/re-spirv), with Khronos SPIRV-Headers | SPIR-V shader specialisation | MIT; SPIRV-Headers under Khronos's MIT-style license | `lib/RT64/src/contrib/re-spirv/LICENSE`, `.../external/SPIRV-Headers/LICENSE` |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers), [volk](https://github.com/zeux/volk), [VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | the Vulkan backend | Apache-2.0, MIT, MIT | `lib/RT64/src/contrib/plume/contrib/{Vulkan-Headers/LICENSE.md, volk/LICENSE.md, VulkanMemoryAllocator/LICENSE.txt}` |
| [D3D12MemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator) | the D3D12 backend, Windows | MIT | `lib/RT64/src/contrib/plume/contrib/D3D12MemoryAllocator/LICENSE.txt` |
| [utf8conv](https://github.com/GiovanniDicanio/UTF8Conv) (Giovanni Dicanio) | UTF-8/UTF-16 conversion in RT64, Windows | MIT | `licenses/utf8conv.txt` (from upstream; the copy in RT64 carries only a copyright line) |
| [ares](https://github.com/ares-emulator/ares) RSP vector unit | librecomp's RSP instruction emulation, compiled with the audio microcode | ISC | `licenses/ares-rsp.txt` |
| [odashi/encoding](https://github.com/odashi/encoding) | EUC-JP conversion in librecomp | MIT | `licenses/odashi-encoding.txt` |
| metal-cpp | the Metal backend, macOS only | Apache-2.0 | `lib/RT64/src/contrib/plume/contrib/metal-cpp/LICENSE.txt` |

### Fonts

| Component | Role | License | Text |
|---|---|---|---|
| Lato, Noto Emoji | menu fonts, copied from RmlUi's samples at build time | SIL Open Font License 1.1 | `lib/RecompFrontend/recompui/lib/RmlUi/Samples/assets/LICENSE.txt` |
| [PromptFont](https://github.com/Shinmera/promptfont) (Yukari "Shinmera" Hafner) | controller and keyboard glyphs in the menus | SIL Open Font License 1.1 | `assets/promptfont/LICENSE.txt` |

On Linux, SDL2, FreeType, GTK 3, HarfBuzz, zlib and the Vulkan loader are the
distribution's own shared libraries, linked at run time and not included in the
archive.

## Open points

These are recorded rather than resolved; each needs an answer from someone other
than this project's code.

- **RecompFrontend publishes no license** (below).
- **The DirectX Shader Compiler's Microsoft terms** permit redistributing the
  DLLs with an application on conditions -- among them that end users agree to
  terms protecting Microsoft, and that the DLLs not be made subject to a license
  requiring source disclosure. The executable they ship beside is a GPL-3.0
  combined work. The other N64: Recompiled ports ship the same DLLs the same way;
  whether that satisfies both licenses has not been established here.
- **Two RT64 shaders are adapted from Shadertoy** (`lib/RT64/src/shaders/BicubicScalingCS.hlsl`
  from shadertoy.com/view/XtKfRV, `DebugPS.hlsl` from shadertoy.com/view/3dlGDM),
  whose default terms are CC BY-NC-SA 3.0 unless their authors said otherwise.
  `GaussianFilterRGB3x3CS.hlsl` is from Microsoft's DirectX-Graphics-Samples (MIT)
  and `rt64_math.cpp` adapts part of glm (MIT). These are RT64's to settle.
- **PromptFont's license file has no copyright line**, upstream as here; the OFL
  expects one. Its author is named above from the upstream repository.
- **The macOS bundle's SDL2, libpng and FreeType** are built from downloaded
  sources by `tools/build_macos_dependencies.sh`; SDL2's and FreeType's texts are
  in `licenses/`, libpng's (libpng-2.0) is not yet included.

## What this means for a built executable

The runtime is GPL-3.0 and is linked statically, so **any executable built from
this project is a combined work distributed under the GNU General Public
License, version 3**, whatever the license of the project's own files. Anyone
who distributes such a binary must make its complete corresponding source
available under the GPL's terms; this repository, at the commit the binary was
built from, together with the submodules it pins, is that source.

RecompFrontend publishes no license. Without one, no permission to copy or
redistribute it exists beyond what its authors grant; this project uses it as a
submodule, as the other N64: Recompiled projects do, and does not vendor a copy.
Anyone intending to distribute a built executable should resolve this with the
RecompFrontend authors first.

## The reference decompilation

The build pipeline uses the [Wave Race 64 decompilation](https://github.com/LLONSIT/Wave-Race-64)
by LLONSIT and contributors as its source of function names, segment layout
and splat configuration. That repository publishes no license. Nothing copied
from it is committed here: the builder clones it into `reference/`, which is
ignored, and the splat config this project derives from its config
(`recomp/wr64.us.rev1.asm.yaml`) is generated on the builder's machine and
ignored too. The function names that appear in this project's own sources and
documents are used as identifiers for the routines they name.

## Artwork

The launcher background (`assets/icons/Logo.svg`), the executable's icon
generated from it, and the menu icons in `assets/icons/` were created for this
project with Claude, Anthropic's AI model, at the direction of the project's
author, and are released under the project's MIT License to the extent that
rights in them exist. Whether purely AI-generated images attract copyright at
all is unsettled and varies by jurisdiction; no claim is made beyond what the
law allows, and nothing in them is taken from the game.

## What is not here

No Nintendo code or data. The recompiled game code in `RecompiledFuncs/` is
generated on the builder's machine from the builder's own dump and is never
committed; the built executable contains that generated code, and loads the
game's assets from the player's dump at run time.
