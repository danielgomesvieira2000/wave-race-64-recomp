#!/usr/bin/env bash
# Are the R_MIPS_PC16 relocations in the overlay sections safe to discard?
#
# A PC-relative branch needs no load-time fixup when its target sits in the same
# section, because relocating the section moves source and destination together
# and the distance between them is unchanged. That is only true if every one of
# them is section-local, which is what this checks rather than assumes.
#
# They exist at all because splat declares functions with `glabel`, making them
# global, and GNU as emits a relocation for a branch to a global symbol even
# when it resolves within the same section.
set -euo pipefail

cd /mnt/c/Users/Daniel/claude-projects/n64recomp_waverace64

mips-linux-gnu-readelf -rW wr64.elf | awk '
    /^Relocation section/ {
        sec = $3
        gsub(/[^A-Za-z0-9_.]/, "", sec)
        sub(/^\.rel/, "", sec)
    }
    $3 == "R_MIPS_PC16" && sec ~ /ovl_|seg_1C3|segment_1B1FB0/ {
        print sec, $1, $5
    }
' > /tmp/pc16.txt

echo "PC16 relocations in overlay sections: $(wc -l < /tmp/pc16.txt)"
echo
echo "target symbol -> section it is defined in:"
while read -r sec off sym; do
    [ -z "$sym" ] && continue
    ndx=$(mips-linux-gnu-readelf -sW wr64.elf | awk -v s="$sym" '$8 == s { print $7; exit }')
    secname=$(mips-linux-gnu-readelf -SW wr64.elf | awk -v n="$ndx" '
        match($0, /\[ *([0-9]+)\]/, m) && m[1] == n { print $2; exit }')
    if [ "$secname" == ".$sec" ]; then
        verdict="same section"
    else
        verdict="DIFFERENT SECTION ($secname) -- not safe to drop"
    fi
    printf "  %-18s %-28s %s\n" "$sec" "$sym" "$verdict"
done < /tmp/pc16.txt | sort -u | head -45
