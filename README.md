# Wave Race 64: Recompiled

An in-progress native PC port of **Wave Race 64 (USA) v1.0**, built by
statically recompiling the game's MIPS code to C with
[N64Recomp](https://github.com/N64Recomp/N64Recomp) and running it on
[N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) with
[RT64](https://github.com/rt64/rt64) as the renderer.

> **No game code or assets are distributed by this repository.** Everything here
> is tooling and original source. Building or running anything requires a
> legally obtained Wave Race 64 dump that you supply yourself. This project is
> unofficial and not affiliated with Nintendo.

## Status

**Phase 00 — scaffolding.** The repository builds a ROM identification tool.
The recompiler has not been run and the game does not boot. See
[docs/PLAN.md](docs/PLAN.md) for the phase plan and
[docs/BUILDING.md](docs/BUILDING.md) for build instructions.

| Phase | | Status |
|---|---|---|
| 00 | Ground rules and skeleton | in progress |
| 01 | Split the ROM | not started |
| 02 | First recompile | not started |
| 03 | Runtime harness | not started |
| 04 | Boot bring-up | not started |
| 05 | Graphics and audio correctness | not started |
| 06 | Enhancements and release | not started |

## Target dump

Everything in this project — every splat segment address, every symbol, every
generated function — is pinned to one dump:

| | |
|---|---|
| Title | Wave Race 64 (USA), v1.0 -- the original US release |
| Cartridge ID | `WR`, region `E`, revision `0` |
| Size | 8 MiB |
| Format | `.z64`, big endian |
| Entry point | `0x80046800` |
| Header CRC | `0x7DE11F53 0x74872F9D` |
| sha1 | `887ab588c2ecc64c52fb2065f06b0a1ee4af13dc` |

**This is not the revision the existing Wave Race 64 reverse engineering
targets.** LLONSIT's decomp supports "US, Rev1" only, and both prior
recompilation attempts use Rev A. We target v1.0 anyway, because the evidence
says the revisions share a link layout: the decomp's Rev A `entry` segment sits
at vram `0x80046800`, which is exactly the entry point in the v1.0 header. Rev A
stays useful as a symbol donor. See [docs/PLAN.md](docs/PLAN.md).

A Rev A or PAL dump will configure, build, and then fail in ways that look like
recompiler bugs. Check yours before starting:

```
WaveRace64Recomp --identify path/to/your.z64
```

## Layout

```
lib/        upstream submodules (N64ModernRuntime, RT64, RecompFrontend)
tools/      python and powershell tooling (committed)
recomp/     N64Recomp configuration and symbol tables
src/        the port's own platform layer
include/    the port's own headers
patches/    RECOMP_PATCH replacements for game functions
docs/       plan and build instructions
```

`RecompiledFuncs/`, `asm/` and any ROM-derived file are generated locally and
are refused by `.gitignore`.

## Credits

The recompilation toolchain is by Mr-Wiseguy and the N64Recomp contributors;
RT64 is by Darío. Prior Wave Race 64 reverse engineering by LLONSIT, WACOMalt
and chronic8000.
