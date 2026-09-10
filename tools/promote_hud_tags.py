"""Promote the tags you saved in the inspector into the port's built-in table.

The HUD inspector's **Save to hud.json** writes to the per-user settings folder
(`%LOCALAPPDATA%\\WaveRace64Recomp` on Windows), which is outside the repository
and outside every build directory. That is right for a scratchpad -- the tags
follow you between builds while you work -- but it means the tags exist only on
the machine that made them. A release carries nothing.

This copies them into `load_defaults()` in `src/dlrewrite.cpp`, which is compiled
into the executable, so every build and every release has them and no player has
to discover them. Run it, look at the diff, commit.

    python tools/promote_hud_tags.py            # promote
    python tools/promote_hud_tags.py --dry-run  # show what it would do
    python tools/promote_hud_tags.py --file X    # read a hud.json from elsewhere
    python tools/promote_hud_tags.py --clear     # ... and empty the local file

Only the block between the two markers is rewritten, so the hand-written entries
above it -- the ones with a paragraph saying why they exist -- are left alone.
Running it twice makes no further change.

**Identities are lower-cased.** `Tags::lookup` lower-cases what it is given, so
an entry written with an upper-case hex digit is never found. This does it for
you; that bug has been made once already.

`--clear` empties the local `hud.json` after promoting. Worth doing once the tags
are in the code: a local file that repeats them will keep overriding the port
even after the built-in entry is changed, and a stale local tag masking a
classifier change is a confusing afternoon.
"""

import argparse
import json
import os
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TARGET = REPO / "src" / "dlrewrite.cpp"

BEGIN = "        // ---- promoted from hud.json by tools/promote_hud_tags.py ----"
END = "        // ---- end promoted ----"

PREAMBLE = [
    BEGIN,
    "        // Tagged in the inspector and promoted here so a release carries them.",
    "        // Everything between these two markers is rewritten by that script;",
    "        // hand-written entries go above the first marker, with their reasons.",
]

CLASSES = {
    "center": "Class::Auto",
    "left": "Class::Left",
    "right": "Class::Right",
    "stretch": "Class::Stretch",
}


def settings_directory():
    """The same directory wr64::settings_directory() picks (src/main.cpp)."""
    if Path("portable.txt").exists():
        return Path.cwd()
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA")
        if base:
            return Path(base) / "WaveRace64Recomp"
    else:
        base = os.environ.get("XDG_DATA_HOME")
        if base:
            return Path(base) / "WaveRace64Recomp"
        home = os.environ.get("HOME")
        if home:
            return Path(home) / ".local" / "share" / "WaveRace64Recomp"
    return Path.cwd()


def read_tags(path):
    if not path.exists():
        sys.exit(f"no hud.json at {path}\n"
                 f"Press F1 in the game, tag something, and press Save to hud.json.")
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except ValueError as e:
        sys.exit(f"{path} is not valid JSON: {e}")
    if not isinstance(doc, dict):
        sys.exit(f"{path} does not hold an object")

    tags = []
    for name in CLASSES:
        for identity in doc.get(name, []) or []:
            if isinstance(identity, str) and identity.strip():
                tags.append((identity.strip().lower(), name))
    # A stable order so the diff is about what changed, not about dict order.
    tags.sort()
    return doc, tags


def build_block(tags):
    lines = list(PREAMBLE)
    if tags:
        width = max(len(i) for i, _ in tags)
        for identity, name in tags:
            key = '"%s"]' % identity
            lines.append("        by_identity[%-*s = %s;" % (width + 3, key, CLASSES[name]))
    else:
        lines.append("        // (nothing promoted yet)")
    lines.append(END)
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--file", type=Path, help="a hud.json to read instead of the local one")
    ap.add_argument("--dry-run", action="store_true", help="print the block, change nothing")
    ap.add_argument("--clear", action="store_true", help="empty the local hud.json afterwards")
    args = ap.parse_args()

    source = args.file if args.file is not None else settings_directory() / "hud.json"
    doc, tags = read_tags(source)
    print(f"  {source}: {len(tags)} tag(s)")

    text = TARGET.read_text(encoding="utf-8")
    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(newline)

    # Warn about an identity the hand-written entries already name: the promoted
    # block comes after them, so the promoted class is the one that will win.
    above = newline.join(lines[: lines.index(BEGIN)]) if BEGIN in lines else text
    for identity, name in tags:
        if f'"{identity}"' in above.split(BEGIN)[0]:
            print(f"  note: {identity} is already set above the marker; "
                  f"the promoted {name} will win")

    block = build_block(tags)
    if BEGIN in lines and END in lines:
        start, stop = lines.index(BEGIN), lines.index(END)
        lines[start : stop + 1] = block
    else:
        anchor = "    void load_defaults() {"
        if anchor not in lines:
            sys.exit(f"load_defaults() not found in {TARGET}; it has been renamed "
                     f"and this script needs revisiting")
        closing = lines.index(anchor)
        while lines[closing] != "    }":
            closing += 1
        lines[closing:closing] = [""] + block

    updated = newline.join(lines)
    if args.dry_run:
        print()
        print(newline.join(block))
        return
    if updated == text:
        print(f"  {TARGET.name} already up to date")
    else:
        TARGET.write_text(updated, encoding="utf-8")
        print(f"  {TARGET.name} updated -- review the diff and commit")

    if args.clear:
        for name in CLASSES:
            doc[name] = []
        source.write_text(json.dumps(doc, indent=4) + "\n", encoding="utf-8")
        print(f"  {source} emptied")

    print("\nRebuild to pick it up.")


if __name__ == "__main__":
    main()
