# Contributing

Patches, bug reports and findings are welcome. Read this first, because one
rule here is absolute and mistakes under it are expensive to undo.

## The rule that has no exceptions

**No copyrighted game material enters this repository.** Not the ROM, not
anything taken out of it, not anything made from what was taken out of it. The
project ships tooling and original code; the user supplies their own dump at
build time and the game's assets are read from it at runtime.

| Material | Where it lives | Why |
|---|---|---|
| A dump (`.z64`, `.n64`, `.v64`), whole or in part, compressed or renamed | your machine only | it is Nintendo's |
| Anything the pipeline derives from a dump: `asm/`, `bin/`, `RecompiledFuncs/`, `wr64.elf`, `.sym`, `.map`, `dump.toml`, `data_dump.toml` | generated locally, already in `.gitignore` | a derivative of the ROM is the ROM |
| Assets extracted from a dump: textures, models, audio, sequences, text, course data | nowhere in this repo | Nintendo's, individually |
| Assets **derived** from those: upscaled or AI-enhanced textures, retouched sprites, remastered audio, geometry re-modelled from the game's | nowhere in this repo | a derivative work of copyrighted art is still copyrighted, however it was processed |
| Screenshots and video of the game running | attached to the issue or PR, not committed | keeps frames of the game's art out of the tree and out of the history |
| Code copied from the [reference decompilation](https://github.com/LLONSIT/Wave-Race-64) | nowhere in this repo | it publishes no license, so no permission to copy it exists |
| The splat config derived from that decompilation's (`recomp/wr64.us.rev1.asm.yaml`) | generated on your machine, `.gitignore`d | same |

This is a repository policy, not a legal opinion about any particular file. It
is drawn wide on purpose: the cost of arguing each case is higher than the cost
of keeping everything on the far side of the line.

## What you *can* write down

Facts about the ROM are the point of this project's documentation, and they are
not the ROM:

- addresses, segment and overlay layouts, sizes, offsets, alignment
- symbol and function names, and what a routine does
- struct fields, table formats, jump-table indices, enum values
- constants you had to determine, and the measurements that determined them

That is what [docs/GAME-INTERNALS.md](docs/GAME-INTERNALS.md) is made of. What
does not belong is **bulk data**: a pasted byte dump, a captured display list of
any length, a table of the game's data transcribed out of a dump. Short excerpts
are fine where they are needed to document a format, and never long enough to
reconstruct anything.

## Check before you push

`.gitignore` is the first line of defence, not a guarantee. `git add -f`, a
renamed extension, a `.zip`, or a path the rules do not cover all get through
it. Check what you are actually about to commit:

```powershell
# staged files, and anything unusually large among them
git diff --cached --name-status
git diff --cached --name-only --diff-filter=ACM |
    ForEach-Object { Get-Item $_ -ErrorAction SilentlyContinue } |
    Where-Object Length -gt 1MB | Select-Object FullName, Length
```

```bash
# the whole branch: the biggest objects it adds on top of main
git rev-list --objects main..HEAD |
    git cat-file --batch-check='%(objecttype) %(objectname) %(objectsize) %(rest)' |
    sort -k3 -n -r | head -20
```

Nothing this project tracks is large. A megabyte of anything in a diff is worth
a second look.

**If a dump or an extracted asset does land in a commit, say so immediately and
do not try to fix it with another commit.** Deleting a file in a later commit
does not remove it: the object stays in the history, and in every clone and fork
that has fetched it. The branch gets rebuilt clean from a fresh checkout and the
original one is abandoned. That is cheap while the branch is still yours and
painful once it is merged, which is why the check above is worth the ten seconds.

## Texture packs, "HD" projects and mods

The port has no texture-replacement or mod system today. If one is added, its
content will load from a folder on the user's machine, and this repository will
still contain none of it.

Upscales are the case worth naming, because they are the most common way this
rule gets broken by accident. A texture upscaled from a rip is a derivative of
Nintendo's artwork; running it through an upscaler does not make it yours or
ours. It cannot be committed here, bundled into a release, or attached to an
issue. A tool that generates a pack **on the user's machine from the user's own
dump** is a different thing, and a link to one is fine.

Original artwork made for the port -- launcher icons, menu graphics, anything
drawn from scratch rather than from the game -- is welcome under `assets/` with
its origin stated in the pull request.

## Third-party code

| Situation | What to do |
|---|---|
| You need a library | add it as a submodule under `lib/`, and add a row to [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) naming its role, license and license-text path |
| The library publishes no license | it cannot be used |
| You need a change inside an upstream submodule (N64Recomp, N64ModernRuntime, RT64, RSPRecomp) | write it as a script in `tools/` (`patch_rt64.py`, `patch_librecomp.py`, ...); the submodules stay pinned to upstream commits and are never committed with a dirty tree |
| Upstream should have the change permanently | send it upstream as well, and say so in the pull request |

Vendoring a copy of someone's source into this tree is not how anything here is
built, and a pull request that does it will be asked to use a submodule instead.

## Working in this repo

Start with [docs/BUILDING.md](docs/BUILDING.md) and get a build running before
changing anything; most of the pipeline's failure modes are easier to recognise
once you have seen it work. [docs/PLAN.md](docs/PLAN.md) is the phase plan and
the standing constraints, [docs/PORTING.md](docs/PORTING.md) explains the
toolchain and runtime symptom-first, and the `docs/PHASE0*-FINDINGS.md` files are
the working record, including the wrong turns.

| Directory | Contents |
|---|---|
| `src/`, `include/` | the port: platform layer, renderer binding, input, audio, launcher wiring |
| `patches/` | replacements and wrappers for individual game functions |
| `recomp/` | N64Recomp configuration |
| `tools/` | the build pipeline's scripts, the upstream patch scripts, and diagnostics |
| `assets/` | the launcher's stylesheet, icons and fonts -- original artwork only |
| `docs/` | build guide, technical reference, plan, findings |

Standing rules, from `docs/PLAN.md`:

- **Generated code is never hand-edited.** If recompiler output is wrong, fix the
  configuration or write a script in `tools/`. A hand-edit disappears at the next
  regeneration and hides the real problem until it does.
- **The technical reference is kept current.** A change that discovers a fact
  about the game updates `docs/GAME-INTERNALS.md`; a change that fixes or
  explains something in the toolchain, runtime or renderer updates
  `docs/PORTING.md` -- in the same commit, not as a follow-up. The phase findings
  are history and stay as they are.
- **Rev A only.** Every address in the project is tied to the USA Rev A dump
  (sha1 `508dfc2d4caa42b6f6de5263d0aed5e44ac7966a`). Support for another revision
  is a design discussion before it is a patch.
- A change that could plausibly make things worse on someone else's machine gets
  an environment-variable switch to turn it off, as `WR64_NO_SKY_INTERP` and
  `WR64_NO_WATER_INTERP` do, so a bug report can bisect it in one run.
- Match the file you are editing: its naming, its comment density, its idiom.

## Commits and pull requests

Commit subjects are a sentence saying what changed; the body says *why*, and what
was measured or observed. `git log` is the model -- the messages there are long
because the reasoning is the part that is hard to recover later.

One change per pull request. Say what you tested: which mode, which course and
direction, at what settings. "It still boots" is not a test of a rendering
change. If you used an AI to write it, that is fine -- this entire project was
written that way -- and it does not move responsibility: the code is yours to
have read, and the two rules above are yours to have checked.

## Reporting a bug

- Run `WaveRace64Recomp.exe --identify your.z64` and paste the result. Anything
  but USA Rev A is the answer to most bug reports.
- Attach `%LOCALAPPDATA%\WaveRace64Recomp\wr64.log`.
- Say the mode, course, direction, the settings in use (Framerate, Widescreen,
  HUD Placement) and whether it happens every time.
- Screenshots and clips as attachments on the issue, never as files in the repo.
- **Never attach a dump**, or any part of one, to an issue.

## Licensing of what you contribute

Contributions to this project's own files (`src/`, `include/`, `patches/`,
`tools/`, `recomp/`, `assets/`, `docs/`) are under the MIT License in `LICENSE`,
and by opening a pull request you confirm you have the right to contribute them
under it. A built executable is a GPL-3.0 combined work, for the reasons set out
in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md); read that before
distributing binaries.
