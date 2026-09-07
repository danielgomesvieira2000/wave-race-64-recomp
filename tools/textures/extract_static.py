#!/usr/bin/env python3
"""Conservatively replay authored texture loads and produce RT64 v5 dumps.

This supplements (and does not replace) runtime capture. It follows straight
line F3DEX regions and known G_DL calls, resolves course DMA segments from
inventory_rom.py, and tracks every initialized TMEM byte. Conditional branches,
unresolved addresses and inherited palettes are never guessed. A region's inherited TLUT mode is accepted
only when its exact replayed hash occurs in a supplied runtime dump. Unobserved
regions retain conditional hashes in report.json rather than entering the pack.

RT64's checked-in C++ TMEMHasher is compiled as a tiny local helper, so its v5
hash implementation is not reimplemented. Its actual hash reads are checked
against a known-byte mask. The loader below translates rt64_rdp.cpp. Source
assets and outputs must remain in ignored build directories, never source git.
"""
from __future__ import annotations

import argparse
import collections
import ctypes
import hashlib
import json
from pathlib import Path
import platform
import re
import struct
import subprocess

from decode_tmem import decode_tmem, inventory as decode_inventory, write_png
from inventory_rom import ROM_SHA1, decode_mio0

ROOT = Path(__file__).resolve().parents[2]
TILE_FIELDS = ("fmt", "siz", "line", "tmem", "palette", "cms", "cmt", "masks",
               "maskt", "shifts", "shiftt", "uls", "ult", "lrs", "lrt")
TLUT = {"None": 0, "RGBA16": 0x8000, "IA16": 0xC000}
LEGAL = {0x00, 0x01, 0x03, 0x04, 0x06, *range(0xB0, 0xC0), *range(0xE4, 0xF0),
         0xF0, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF}
DRAW = {0xBF, 0xB5, 0xB1, 0xE4, 0xE5}
BOUNDARY = {0x06, 0xB0, 0xB8}


class Unresolved(ValueError):
    pass


def words(data, offset):
    return struct.unpack_from(">II", data, offset)


def valid_command(data, offset):
    """Reject vertex/image bytes whose first byte merely resembles an opcode."""
    w0, w1 = words(data, offset)
    op = w0 >> 24
    if op not in LEGAL:
        return False
    if op in (0, 0xB8, 0xE6, 0xE7, 0xE8, 0xE9):
        return w0 & 0xFFFFFF == 0 and w1 == 0
    if op in (0xB6, 0xB7, 0xBF, 0xB5, 0xF8, 0xF9, 0xFB):
        return w0 & 0xFFFFFF == 0
    if op in (0xBA, 0xB9):
        shift, length = w0 >> 8 & 255, w0 & 255
        return w0 & 0xFF0000 == 0 and 0 < length <= 32 and shift + length <= 32
    if op == 0xBC:
        return w0 & 255 in (2, 4, 6, 8, 10, 12, 14)
    if op in (0xFD, 0xFF):
        return w0 >> 21 & 7 <= 4 and w0 & 0x7F000 == 0
    if op == 0xF5:
        return w0 >> 21 & 7 <= 4 and w1 & 0xF8000000 == 0
    if op in (0xF0, 0xF2, 0xF3, 0xF4):
        return w1 & 0xF8000000 == 0
    if op == 0x06:
        return w0 & 65535 == 0 and w0 >> 16 & 255 in (0, 1)
    if op == 0xEF:
        # This game uses F3DEX BA/B9 for other-mode fields. Treat raw EF as
        # an unsupported boundary; otherwise signed vertex coordinates such
        # as EF6BFFC4 would invent an explicit TLUT setting before a real list.
        return False
    return True


def tile_descriptor(w0, w1):
    return dict(zip(TILE_FIELDS, (w0 >> 21 & 7, w0 >> 19 & 3, w0 >> 9 & 511,
        w0 & 511, w1 >> 20 & 15, w1 >> 8 & 3, w1 >> 18 & 3, w1 >> 4 & 15,
        w1 >> 14 & 15, w1 & 15, w1 >> 10 & 15, 0, 0, 0, 0)))


def sample_dimensions(tile):
    """State::loadDrawState tile/mask/clamp calculation, not load byte count."""
    result = []
    for mask, clamp, low, high in (("masks", "cms", "uls", "lrs"), ("maskt", "cmt", "ult", "lrt")):
        tile_size = max((tile[high] - tile[low] + 4) // 4, 1) if not tile[mask] or tile[clamp] & 2 else 65535
        mask_size = 1 << tile[mask] if tile[mask] else 65535
        result.append(min(tile_size, mask_size))
    return tuple(result)


class Hasher:
    """Use the actual checked-in v5 hasher, also instrumenting its byte reads."""
    def __init__(self, output):
        output.mkdir(parents=True, exist_ok=True)
        source = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#define XXH_INLINE_ALL
#include "contrib/xxHash/xxhash.h"
#include "shared/rt64_f3d_defines.h"
namespace RT64 { struct LoadTile {
uint8_t fmt,siz; uint16_t line,tmem; uint8_t palette,cms,cmt,masks,maskt,shifts,shiftt;
uint16_t uls,ult,lrs,lrt; }; }
static const uint8_t *base, *known;
static bool valid;
static XXH_errorcode checkedUpdate(XXH3_state_t *state, const void *data, size_t n) {
    auto p=reinterpret_cast<uintptr_t>(data), b=reinterpret_cast<uintptr_t>(base);
    if (p >= b && p < b+4096) {
        if (p+n>b+4096) { valid=false; return XXH_ERROR; }
        for(size_t i=0;i<n;i++) if (!known[p-b+i]) valid=false;
    }
    return XXH3_64bits_update(state,data,n);
}
#define XXH3_64bits_update checkedUpdate
#include "common/rt64_tmem_hasher.h"
extern "C" uint64_t hash_v5(const uint8_t *memory,const uint8_t *initialized,
 const uint32_t *v,uint16_t width,uint16_t height,uint32_t tlut,int *ok) {
 RT64::LoadTile t{};
 t.fmt=v[0];t.siz=v[1];t.line=v[2];t.tmem=v[3];t.palette=v[4];
 t.cms=v[5];t.cmt=v[6];t.masks=v[7];t.maskt=v[8];t.shifts=v[9];t.shiftt=v[10];
 t.uls=v[11];t.ult=v[12];t.lrs=v[13];t.lrt=v[14];
 base=memory;known=initialized;valid=true;
 if(RT64::TMEMHasher::requiresRawTMEM(t,width,height,tlut)) {*ok=0;return 0;}
 auto hash=RT64::TMEMHasher::hash(memory,t,width,height,tlut,5);
 *ok=valid;return hash;
}
'''
        source_path, library_path = output / "hash_helper.cpp", output / "hash_helper.so"
        source_path.write_text(source)
        command = ["c++", "-std=c++17", "-O2", "-I", str(ROOT / "lib/RT64/src")]
        command += ["-dynamiclib"] if platform.system() == "Darwin" else ["-shared", "-fPIC"]
        subprocess.run(command + [str(source_path), "-o", str(library_path)], check=True, capture_output=True)
        self.library = ctypes.CDLL(str(library_path.resolve()))
        self.function = self.library.hash_v5
        self.function.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32),
                                  ctypes.c_uint16, ctypes.c_uint16, ctypes.c_uint32, ctypes.POINTER(ctypes.c_int)]
        self.function.restype = ctypes.c_uint64

    def __call__(self, tmem, known, info):
        values = (ctypes.c_uint32 * len(TILE_FIELDS))(*(info["tile"][key] for key in TILE_FIELDS))
        ok = ctypes.c_int()
        result = self.function(bytes(tmem), bytes(known), values, info["width"], info["height"], TLUT[info["tlut"]], ctypes.byref(ok))
        if not ok.value:
            raise Unresolved("uninitialized_hashed_tmem_or_raw_tmem")
        return f"{result:016x}"


def load_tmem(tmem, known, source, tile, image, opcode):
    """RDP::load{Block,Tile,TLUT}Operation / loadToTMEMCommon byte translation.

    Input bytes are canonical big-endian ROM/MIO0 bytes, so the RDRAM host XOR3
    in the C++ loader has already been accounted for. All odd-row swaps remain.
    Bounds are proved before the first write: a partial failed load cannot leak.
    """
    block, tlut = opcode == 0xF3, opcode == 0xF0
    rgba32 = tile["fmt"] == 0 and tile["siz"] == 3
    row_stride = (image["width"] << image["siz"]) >> 1
    if block:
        start = ((tile["uls"] << image["siz"]) >> 1) + row_stride * tile["ult"]
        row_count = 1
        word_count = ((tile["lrs"] - tile["uls"]) >> (4 - tile["siz"])) + 1
    else:
        start = (((tile["uls"] >> 2) << image["siz"]) >> 1) + row_stride * (tile["ult"] >> 2)
        row_count = 1 + (tile["lrt"] >> 2) - (tile["ult"] >> 2)
        span = (tile["lrs"] >> 2) - (tile["uls"] >> 2)
        word_count = span + 1 if tlut else (span >> (4 - tile["siz"])) + 1
    advance, source_advance = (4 if rgba32 else 8), (2 if tlut else 8)
    stride, mask = tile["line"] << (5 if tlut else 3), (2047 if rgba32 else 4095)
    end = start + (row_count - 1) * row_stride + word_count * source_advance
    if row_count < 1 or word_count < 1 or start < 0 or end > len(source):
        raise Unresolved("source_load_out_of_bounds")
    if word_count * row_count > 8192:
        raise Unresolved("excessive_load_size")
    # Large-row skip optimization in RT64 is deliberately rejected instead of
    # pretending its atypical wrap behavior is the ordinary sequential loader.
    if stride and word_count * advance <= stride and row_count > (mask + stride) // stride:
        raise Unresolved("oversized_load_uses_rt64_row_skip")
    row_tmem, xor_mask, dxt = (tile["tmem"] << 3) & mask, 0, 0
    for y in range(row_count):
        ta, sa = row_tmem, start + y * row_stride
        for _ in range(word_count):
            if rgba32:
                locations = ((0, 0, 0), (1, 1, 0), (2, 4, 0), (3, 5, 0),
                             (0, 2, 2048), (1, 3, 2048), (2, 6, 2048), (3, 7, 2048))
            else:
                locations = ((i, i, 0) for i in range(8))
            for dest_offset, src_offset, bank in locations:
                destination = ((ta + dest_offset) ^ xor_mask) | bank
                tmem[destination] = source[sa + (src_offset & (1 if tlut else 7))]
                known[destination] = 1
            if block:
                dxt += tile["lrt"]
                while dxt >= 0x800:
                    ta = (ta + stride) & mask
                    dxt -= 0x800
                    xor_mask ^= 4
            sa += source_advance
            ta = (ta + advance) & mask
        row_tmem = (row_tmem + stride) & mask
        xor_mask ^= 4


def region_for(data, offset):
    """Maximal uninterrupted command region; calls do not inherit guessed state."""
    start = offset
    while start >= 8:
        op = data[start - 8]
        if not valid_command(data, start - 8) or op in BOUNDARY:
            break
        start -= 8
    end = offset
    while end + 8 <= len(data):
        op = data[end]
        if not valid_command(data, end) or op in BOUNDARY:
            break
        end += 8
    return start, end


def graph_start(data, offset):
    start = offset
    while start >= 8:
        op = data[start - 8]
        if not valid_command(data, start - 8) or op in (0xB0, 0xB8):
            break
        start -= 8
    return start


def flatten_graph(data, start, segments, bank_names, max_depth=32, max_commands=32768):
    """Follow only fully resolved G_DL control flow, preserving call state.

    A true tail branch does not return to its caller. Vertex/cull commands are
    retained: continuing after G_CULLDL represents the valid visible path.
    Conditional G_BRANCH_Z and segment mutations cannot be resolved offline.
    """
    output, locations, calls, stack = bytearray(), [], [], set()

    def walk(current, pc, depth):
        if depth > max_depth:
            raise Unresolved("display_list_depth_limit")
        origin = (id(current), pc)
        if origin in stack:
            raise Unresolved("display_list_cycle")
        stack.add(origin)
        try:
            while pc + 8 <= len(current):
                if len(locations) >= max_commands:
                    raise Unresolved("display_list_command_limit")
                w0, w1 = words(current, pc)
                op = w0 >> 24
                if op == 0xB8:
                    return
                if not valid_command(current, pc):
                    raise Unresolved("display_list_unrecognized_command")
                if op == 0xB0:
                    raise Unresolved("display_list_conditional_branch")
                if op == 0xBC and w0 & 255 == 6:
                    raise Unresolved("display_list_segment_mutation")
                if op == 0x06:
                    if w0 & 65535 or w0 >> 16 & 255 not in (0, 1):
                        raise Unresolved("invalid_display_list_command")
                    segment, offset = w1 >> 24, w1 & 0xFFFFFF
                    if segment not in segments:
                        raise Unresolved(f"unresolved_display_list_segment_{segment}")
                    target, base = segments[segment]
                    offset -= base
                    if offset < 0 or offset & 7 or offset + 8 > len(target):
                        raise Unresolved("display_list_target_out_of_bounds")
                    calls.append({"bank": bank_names.get(id(current), "unknown"), "offset": pc,
                        "target_bank": bank_names.get(id(target), "unknown"), "target_offset": offset,
                        "tail": bool(w0 >> 16 & 1)})
                    walk(target, offset, depth + 1)
                    if w0 >> 16 & 1:
                        return
                else:
                    output.extend(current[pc:pc + 8])
                    locations.append({"bank": bank_names.get(id(current), "unknown"), "offset": pc})
                pc += 8
            raise Unresolved("display_list_missing_end")
        finally:
            stack.remove(origin)

    walk(data, start, 0)
    return bytes(output), locations, calls


def replay_region(data, start, end, segments):
    tiles, sized = {}, set()
    tmem, known = bytearray(4096), bytearray(4096)
    image, active_tile, mode, epoch = None, None, None, 0
    pending, snapshots, errors = [], [], []
    for pos in range(start, end, 8):
        w0, w1 = words(data, pos)
        op = w0 >> 24
        if op == 0xF5:
            index = w1 >> 24 & 7
            prior = tiles.get(index)
            tile = tile_descriptor(w0, w1)
            if prior:
                tile.update({key: prior[key] for key in ("uls", "ult", "lrs", "lrt")})
            tiles[index] = tile
        elif op == 0xF2:
            index = w1 >> 24 & 7
            if index in tiles:
                tiles[index].update(uls=w0 >> 12 & 4095, ult=w0 & 4095, lrs=w1 >> 12 & 4095, lrt=w1 & 4095)
                sized.add(index)
        elif op == 0xFD:
            image = {"fmt": w0 >> 21 & 7, "siz": w0 >> 19 & 3, "width": (w0 & 4095) + 1,
                     "address": w1, "command_offset": pos}
        elif op in (0xF0, 0xF3, 0xF4):
            index = w1 >> 24 & 7
            try:
                if image is None or index not in tiles:
                    raise Unresolved("inherited_load_descriptor")
                segment, offset = image["address"] >> 24, image["address"] & 0xFFFFFF
                if segment not in segments:
                    raise Unresolved(f"unresolved_source_segment_{segment}")
                source, base_offset = segments[segment]
                offset -= base_offset
                if offset < 0 or offset >= len(source):
                    raise Unresolved("source_address_out_of_bounds")
                tile = dict(tiles[index], uls=w0 >> 12 & 4095, ult=w0 & 4095, lrs=w1 >> 12 & 4095, lrt=w1 & 4095)
                # RDP load commands also update the load tile rectangle.
                tiles[index] = tile
                load_tmem(tmem, known, source[offset:], tile, image, op)
                pending.append(image["command_offset"])
            except Unresolved as error:
                known[:] = bytes(4096)
                errors.append({"command_offset": pos, "reason": str(error)})
        elif op == 0xBB:
            active_tile = w0 >> 8 & 7
        elif op == 0xBA:
            shift, length = w0 >> 8 & 255, w0 & 255
            if shift <= 14 and shift + length >= 16:
                mode = {0: "None", 2: "RGBA16", 3: "IA16"}.get(w1 >> 14 & 3)
                epoch += 1
        elif op == 0xEF:
            mode = {0: "None", 2: "RGBA16", 3: "IA16"}.get(w0 >> 14 & 3)
            epoch += 1
        elif op in DRAW and pending:
            draw_tile = w1 >> 24 & 7 if op in (0xE4, 0xE5) else active_tile
            conditional_tile_selection = False
            if draw_tile is None and len(sized) == 1:
                # A complete tile has a mathematically valid texture/hash even
                # when its G_TEXTURE selection is inherited. Keep that source
                # conditional unless the exact hash is observed at runtime.
                draw_tile = next(iter(sized))
                conditional_tile_selection = True
            if draw_tile not in tiles or draw_tile not in sized:
                errors.append({"command_offset": pos, "loads": list(pending), "reason": "inherited_render_tile_or_dimensions"})
            else:
                tile = tiles[draw_tile].copy()
                width, height = sample_dimensions(tile)
                if tile["line"] and width <= 4096 and height <= 4096:
                    snapshots.append({"command_offset": pos, "loads": list(pending), "tile": tile,
                        "width": width, "height": height, "tlut": mode, "mode_epoch": epoch,
                        "conditional_tile_selection": conditional_tile_selection,
                        "conditional_no_local_draw": False,
                        "tmem": bytes(tmem), "known": bytes(known)})
                else:
                    errors.append({"command_offset": pos, "loads": list(pending), "reason": "invalid_or_dynamic_render_size"})
            pending.clear()
    if pending:
        selected = active_tile if active_tile in sized else next(iter(sized)) if len(sized) == 1 else None
        if selected in tiles and selected in sized:
            tile = tiles[selected].copy()
            width, height = sample_dimensions(tile)
            if tile["line"] and width <= 4096 and height <= 4096:
                snapshots.append({"command_offset": max(start, end - 8), "loads": list(pending), "tile": tile,
                    "width": width, "height": height, "tlut": mode, "mode_epoch": epoch,
                    "conditional_tile_selection": selected != active_tile, "conditional_no_local_draw": True,
                    "tmem": bytes(tmem), "known": bytes(known)})
            else:
                errors.append({"command_offset": end, "loads": list(pending), "reason": "no_local_draw_after_load"})
        else:
            errors.append({"command_offset": end, "loads": list(pending), "reason": "no_local_draw_after_load"})
    return snapshots, errors


def dma_placements(rom, inventory):
    """Find exact bank descriptors in the known main-segment data region.

    The four words must name the inventory's exact compressed stream and fit
    its verified end boundary. Only loader flags with a proven destination are
    used; no searching for coincidental image-byte matches to infer offsets.
    """
    by_start = {bank["rom_start"]: bank for bank in inventory["banks"]}
    result = collections.defaultdict(set)
    for pos in range(0x90000, min(0xF6090, len(rom) - 15), 4):
        start, end, flag, offset = struct.unpack_from(">IIII", rom, pos)
        bank = by_start.get(start)
        if (bank and bank["compressed_end"] <= end <= bank["next_stream_boundary"]
                and flag in (1, 3, 4, 5, 7) and offset < 0x100000):
            # Rider flag4 (func_80095CE8) and object flag7 (func_80096048)
            # call func_800967EC to add the same bank placement to every FD
            # image pointer. Before that relocation, both bytes and pointers
            # are bank-local at offset zero. The latter call is also present
            # in the ROM instruction at 0x80096114, not a guessed ASM stub.
            if flag in (4, 7):
                offset = 0
            result[bank["id"]].add((13 if flag == 5 else 8, offset, pos))
    return result


def contexts(inventory, decoded, placements):
    """Explicit course mapping, plus relocatable bank-local segment-8 models."""
    used = set()
    for course in inventory["courses"]:
        for players, key in ((1, "one_player_geometry"), (2, "two_player_geometry")):
            segments = {13 if entry["flag"] == 5 else 14: (decoded[entry["bank"]], entry["destination_offset"])
                        for entry in course[key] if entry["bank"] and entry["flag"] in (5, 8)}
            for entry in course[key]:
                if not entry["bank"] or entry["flag"] not in (5, 8):
                    continue
                bank = entry["bank"]
                used.add(bank)
                yield f"course_{course['id']}_{players}p_{bank}", bank, segments, "course_DMA_flags_5_and_8"
    for bank in inventory["banks"]:
        if bank["id"] not in used and bank["loads"]:
            known = placements.get(bank["id"], ())
            if known:
                unique = {(segment, offset) for segment, offset, _ in known}
                for segment, offset in sorted(unique):
                    yield f"{bank['id']}_at_{segment:x}_{offset:x}", bank["id"], {segment: (decoded[bank["id"]], offset)}, "validated_DMA_bank_descriptor"
                continue
            # Address addends applied by func_800967EC affect both mesh data and
            # its texture pointers; reading the original bank subtracts them.
            yield bank["id"], bank["id"], {8: (decoded[bank["id"]], 0)}, "bank_local_segment_8_pending_runtime_confirmation"


def animated_contexts(items, decoded):
    """Finite bank-backed segment aliases proved by the original game code."""
    for context, bank, segments, mapping in items:
        if context.startswith("course_6_"):
            # func_8006E0F4: scroll advances4 modulo64, sign frame is0,1,2,1.
            # func_8006E674 at ROM PCs80071824..800718D8: segment9/10 use
            # bank14 offsetsF478/11480 + scroll*64; segment11 is13488+frame*4096.
            data = decoded["mio0_23"]
            for scroll in range(0, 64, 4):
                for frame in range(3):
                    aliases = dict(segments)
                    for segment, offset in ((9, 0xF478 + scroll * 64),
                                             (10, 0x11480 + scroll * 64), (11, 0x13488 + frame * 4096)):
                        aliases[segment] = (data[offset:offset + 4096], 0)
                        if len(aliases[segment][0]) != 4096:
                            raise ValueError("Twilight source alias exceeds authored bank")
                    yield f"{context}_scroll{scroll:02d}_frame{frame}", bank, aliases, mapping
        elif bank == "mio0_127":
            # Award state66 selects heap base802F6800 at D_800DCE84[4]. Its
            # DMA list places mio0_at_1AE660 at66000, absolute8035C800.
            # func_80091DBC selects normal/blink pairs of32*64 CI8 +256 RGBA16.
            data = decoded["mio0_at_1AE660"]
            if len(data) != 8 * 0xA00:
                raise ValueError("ceremony face-bank size differs from authored layout")
            seen = set()
            for tick in range(1, 33):
                blink = (tick in (28, 31), tick in (5, 9), tick in (11, 14), tick in (25, 28))
                if blink in seen:
                    continue
                seen.add(blink)
                aliases = dict(segments)
                for index, segment in enumerate((9, 11, 12, 10)):
                    offset = (index * 2 + blink[index]) * 0xA00
                    aliases[segment] = (data[offset:offset + 0xA00], 0)
                yield f"{context}_blink{''.join(str(int(x)) for x in blink)}", bank, aliases, mapping
        else:
            yield context, bank, segments, mapping


def extract(rom, inventory, runtime_dirs, output):
    output.mkdir(parents=True, exist_ok=True)
    dump_dir = output / "dump"
    dump_dir.mkdir(exist_ok=True)
    hasher = Hasher(output / "hash-helper")
    runtime = {}
    hash_checks = collections.Counter()
    for directory in runtime_dirs:
        for path in directory.rglob("*.v5.tile.json"):
            key = path.name.split(".")[0]
            if key in runtime:
                continue
            info = json.loads(path.read_text())
            tmem = path.with_name(path.name.removesuffix(".tile.json") + ".tmem").read_bytes()
            try:
                actual = hasher(tmem, bytes([1]) * 4096, info)
                if actual != key:
                    hash_checks["mismatch"] += 1
                    continue
                hash_checks["matched"] += 1
                runtime[key] = info
            except Unresolved:
                hash_checks["raw_tmem_not_applicable"] += 1
    if hash_checks["mismatch"]:
        raise ValueError("RT64 v5 helper disagrees with runtime dumps; extraction stopped")
    decoded = {bank["id"]: decode_mio0(rom, bank["rom_start"], bank["next_stream_boundary"])[0]
               for bank in inventory["banks"]}
    banks = {bank["id"]: bank for bank in inventory["banks"]}
    conditional_dir = output / "conditional" / "dump"
    conditional_dir.mkdir(parents=True, exist_ok=True)
    emitted, conditional, unresolved, region_reports = {}, {}, [], []
    for context, bank_name, segments, mapping in animated_contexts(contexts(inventory, decoded, dma_placements(rom, inventory)), decoded):
        data = decoded[bank_name]
        variant_only = ("_scroll" in context and not context.endswith("_scroll00_frame0")) or ("_blink" in context and not context.endswith("_blink0000"))
        loads = [load for load in banks[bank_name]["loads"] if not variant_only or load["segment"] in (9, 10, 11, 12)]
        regions = sorted({region_for(data, load["command_offset"]) for load in loads})
        replay_inputs = [(start, end, data, None, []) for start, end in regions]
        graph_roots = set()
        for pos in range(0, 0 if variant_only else len(data) - 7, 8):
            w0, w1 = words(data, pos)
            if w0 >> 24 == 6 and w0 & 65535 == 0 and w0 >> 16 & 255 in (0, 1) and w1 >> 24 in segments:
                graph_roots.add(graph_start(data, pos))
        for start in sorted(graph_roots):
            try:
                graph, locations, calls = flatten_graph(data, start, segments, {id(v): k for k, v in decoded.items()})
                replay_inputs.append((0, len(graph), graph, locations, calls))
            except Unresolved as error:
                unresolved.append({"context": context, "bank": bank_name, "region_start": start,
                    "reason": str(error), "graph_root": True})
        prepared_regions = []
        for start, end, commands, locations, calls in replay_inputs:
            snapshots, errors = replay_region(commands, start, end, segments)
            source_start = locations[0]["offset"] if locations else start
            region_id = f"{context}_{source_start:06x}_{end:06x}" + ("_graph" if locations else "")
            base = {"context": context, "bank": bank_name, "region": region_id,
                    "region_start": source_start, "region_end": end, "segment_mapping": mapping,
                    "call_trace": calls}
            if locations:
                for snap in snapshots:
                    snap["load_locations"] = [locations[offset // 8] for offset in snap["loads"]]
                    snap["command_location"] = locations[snap["command_offset"] // 8]
                for error in errors:
                    if "loads" in error:
                        error["load_locations"] = [locations[offset // 8] for offset in error["loads"]]
            unresolved.extend(dict(base, **error) for error in errors)
            prepared, evidence = [], collections.defaultdict(set)
            for snap in snapshots:
                candidates, failures = [], []
                for mode in (list(TLUT) if snap["tlut"] is None else [snap["tlut"]]):
                    info = {key: snap[key] for key in ("tile", "width", "height")}
                    info["tlut"] = mode
                    try:
                        key = hasher(snap["tmem"], snap["known"], info)
                        candidates.append((key, info))
                        if key in runtime:
                            evidence[snap["mode_epoch"]].add(mode)
                    except Unresolved as error:
                        failures.append(str(error))
                prepared.append((snap, candidates, failures))
            prepared_regions.append((base, prepared, evidence))
        # Bank placement is constant for this context. Exact source/runtime
        # matches calibrate that address mapping, but never propagate TLUT or
        # other draw state across display-list boundaries.
        mapping_confirmed = mapping in ("course_DMA_flags_5_and_8", "validated_DMA_bank_descriptor") or any(
            bool(evidence) for _, _, evidence in prepared_regions)
        for base, prepared, evidence in prepared_regions:
            for snap, candidates, failures in prepared:
                item = dict(base, command_offset=snap["command_offset"], loads=snap["loads"])
                if "load_locations" in snap:
                    item.update(load_locations=snap["load_locations"], command_location=snap["command_location"])
                observed_modes = evidence[snap["mode_epoch"]]
                mode = snap["tlut"] or (next(iter(observed_modes)) if len(observed_modes) == 1 else None)
                accepted = [(key, info) for key, info in candidates if info["tlut"] == mode and mapping_confirmed
                            and (not (snap["conditional_tile_selection"] or snap["conditional_no_local_draw"]) or key in runtime)]
                if not accepted:
                    reason = (failures[0] if not candidates and failures else
                              "unconfirmed_segment_8_mapping" if not mapping_confirmed else
                              "no_local_draw_after_load" if snap["conditional_no_local_draw"] else
                              "inherited_active_render_tile" if snap["conditional_tile_selection"] else
                              "inherited_tlut_mode" if mode is None else
                              failures[0] if failures else "no_valid_candidate")
                    unresolved.append(dict(item, reason=reason,
                        conditional_hashes=[{"hash": key, "tlut": info["tlut"], "runtime_observed": key in runtime}
                                            for key, info in candidates]))
                    if mapping_confirmed:
                        for key, info in candidates:
                            source_item = dict(item, hash=key, metadata=info,
                                tlut_evidence="conditional_mode_only_not_observed", reason=reason)
                            if key not in conditional:
                                rgba = decode_tmem(snap["tmem"], info)
                                pixels_id = hashlib.sha256(struct.pack(">II", info["width"], info["height"]) + rgba).hexdigest()
                                (conditional_dir / f"{key}.v5.tmem").write_bytes(snap["tmem"])
                                (conditional_dir / f"{key}.v5.tile.json").write_text(json.dumps(info, indent=2) + "\n")
                                conditional[key] = {"hash": key, "image_id": pixels_id, "sources": []}
                            conditional[key]["sources"].append(source_item)
                    continue
                key, info = accepted[0]
                item.update(hash=key, runtime_observed=key in runtime, metadata=info,
                    tlut_evidence="explicit_RDP_mode" if snap["tlut"] else "runtime_hash_match_in_same_region_and_mode_epoch")
                if key not in emitted:
                    # Deliberately emit RT64-compatible input dumps. The existing
                    # decoder supplies canonical inventory/deduplication/PNGs.
                    (dump_dir / f"{key}.v5.tmem").write_bytes(snap["tmem"])
                    (dump_dir / f"{key}.v5.tile.json").write_text(json.dumps(info, indent=2) + "\n")
                    emitted[key] = {"hash": key, "runtime_observed": key in runtime, "sources": []}
                emitted[key]["sources"].append(item)
            region_reports.append(dict(base, snapshots=len(prepared), runtime_mode_evidence={str(k): sorted(v) for k, v in evidence.items()}))
    # A rerun must not leave formerly accepted dumps behind as apparent coverage.
    for path in dump_dir.glob("*.v5.*"):
        if path.name.split(".")[0] not in emitted and path.suffix in (".tmem", ".json"):
            path.unlink()
    # A key confirmed elsewhere is not an additional conditional texture.
    conditional = {key: value for key, value in conditional.items() if key not in emitted and key not in runtime}
    runtime_modes = {(info["tile"]["fmt"], info["tile"]["siz"], info["tlut"]) for info in runtime.values()}
    supported_dir = output / "conditional" / "format-supported-dump"
    supported_dir.mkdir(exist_ok=True)
    supported = {}
    for key, entry in conditional.items():
        entry["matches_observed_format_mode"] = any((source["metadata"]["tile"]["fmt"],
            source["metadata"]["tile"]["siz"], source["metadata"]["tlut"]) in runtime_modes for source in entry["sources"])
        if entry["matches_observed_format_mode"]:
            supported[key] = entry
            for extension in ("tmem", "tile.json"):
                name = f"{key}.v5.{extension}"
                (supported_dir / name).write_bytes((conditional_dir / name).read_bytes())
    for path in conditional_dir.glob("*.v5.*"):
        if path.name.split(".")[0] not in conditional and path.suffix in (".tmem", ".json"):
            path.unlink()
    for path in supported_dir.glob("*.v5.*"):
        if path.name.split(".")[0] not in supported and path.suffix in (".tmem", ".json"):
            path.unlink()
    by_bank = []
    for bank in inventory["banks"]:
        sources = [source for entry in emitted.values() for source in entry["sources"] if source["bank"] == bank["id"]
                   or any(loc["bank"] == bank["id"] for loc in source.get("load_locations", []))]
        unresolved_bank = [item for item in unresolved if item["bank"] == bank["id"]]
        conditional_sources = [source for entry in conditional.values() for source in entry["sources"] if source["bank"] == bank["id"]]
        by_bank.append({"bank": bank["id"], "authored_load_candidates": len(bank["loads"]),
            "emitted_hashes": len({source["hash"] for source in sources}),
            "new_hashes": len({source["hash"] for source in sources if not source["runtime_observed"]}),
            "emitted_load_command_offsets": sorted({loc["offset"] for source in sources for loc in
                source.get("load_locations", [{"bank": source["bank"], "offset": offset} for offset in source["loads"]])
                if loc["bank"] == bank["id"]}),
            "conditional_unique_hashes": len({source["hash"] for source in conditional_sources}),
            "unresolved_reasons": dict(collections.Counter(item["reason"] for item in unresolved_bank))})
    report = {"schema_version": 1, "rom_sha1": hashlib.sha1(rom).hexdigest(),
        "summary": {"runtime_hash_checks": dict(hash_checks), "regions": len(region_reports),
            "emitted_hashes": len(emitted), "new_hashes": sum(not v["runtime_observed"] for v in emitted.values()),
            "conditional_unique_hashes": len(conditional),
            "conditional_unique_images": len({entry["image_id"] for entry in conditional.values()}),
            "conditional_format_supported_hashes": len(supported),
            "conditional_format_supported_images": len({entry["image_id"] for entry in supported.values()}),
            "unresolved_entries": len(unresolved), "unresolved_reasons": dict(collections.Counter(v["reason"] for v in unresolved))},
        "limitations": ["Known-bank G_DL calls and tail branches are traversed with depth/cycle/command bounds; unresolved or conditional branches are rejected.",
            "Unknown initial TMEM bytes are tracked and cannot be hashed as zero-filled art.",
            "Inherited TLUT mode needs an exact runtime hash match in the same uninterrupted region and mode epoch.",
            "Conditional dumps have proven bytes and dimensions but an unobserved inherited TLUT mode; they are separate from confirmed dump/.",
            "conditional/format-supported-dump selects only format/TLUT combinations seen elsewhere at runtime. This is a review filter, not proof that a given conditional source was rendered.",
            "Unresolved source address or inherited palette/descriptor counts cannot establish a unique texture count until those fields are known.",
            "Dynamic renderer sizing, raw-TMEM uploads, uncompressed runtime commands and unvisited banks may remain unresolved."],
        "entries": list(emitted.values()), "conditional_entries": list(conditional.values()),
        "unresolved": unresolved, "regions": region_reports, "banks": by_bank}
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def build_addon(report, source_inventory, runtime_dirs, output, base_inventory):
    """Merge new images and join proven adjacent ROM rows with explicit crops."""
    target = output / "addon"
    target.mkdir(exist_ok=True)
    (target / "images").mkdir(exist_ok=True)
    base = json.loads(base_inventory.read_text())
    base_ids = {entry["image_id"] for entry in base["images"]}
    merged = {}
    for index, directory in enumerate([*runtime_dirs, output / "dump", output / "conditional/format-supported-dump"]):
        decoded_dir = output / "addon-decoded" / f"source-{index}"
        data = decode_inventory(directory, decoded_dir)
        (decoded_dir / "inventory.json").write_text(json.dumps(data, indent=2) + "\n")
        if data["summary"]["errors"] or data["summary"]["orphaned_tmem_files"]:
            raise ValueError("addon source decoding failed")
        for entry in data["images"]:
            ident = entry["image_id"]
            if ident not in merged:
                merged[ident] = dict(entry, png=str((decoded_dir / entry["png"]).resolve()))
                merged[ident]["hash_aliases"] = []
            merged[ident]["hash_aliases"] = sorted(set(merged[ident]["hash_aliases"]) | set(entry["hash_aliases"]))
    additions = {key: value for key, value in merged.items() if key not in base_ids}
    by_hash = {key: entry for entry in additions.values() for key in entry["hash_aliases"]}
    all_by_hash = {key: entry for entry in merged.values() for key in entry["hash_aliases"]}
    banks = {bank["id"]: {load["command_offset"]: load for load in bank["loads"]} for bank in source_inventory["banks"]}
    wide = {}
    for entry in report["entries"] + report["conditional_entries"]:
        if entry["hash"] not in by_hash:
            continue
        for source in entry["sources"]:
            info = source["metadata"]
            if info["width"] < 512 or info["height"] > 16 or info["tlut"] != "None":
                continue
            locations = source.get("load_locations", [{"bank": source["bank"], "offset": offset} for offset in source["loads"]])
            descriptors = [(loc["bank"], banks.get(loc["bank"], {}).get(loc["offset"])) for loc in locations]
            descriptors = [(bank, load) for bank, load in descriptors if load and load["kind"] != "palette"]
            if len(descriptors) != 1:
                continue
            bank, load = descriptors[0]
            # Only block-loaded RGBA/I/IA rows whose original byte count agrees
            # with the render dimensions participate in a contiguous join.
            byte_count = (info["width"] * info["height"] * (4 << info["tile"]["siz"]) + 7) // 8
            record = {"hash": entry["hash"], "image_id": by_hash[entry["hash"]]["image_id"],
                "source_bank": bank, "source_command_offset": load["command_offset"],
                "source_address": load["image_address"], "source_offset": load["segment_offset"],
                "width": info["width"], "height": info["height"], "fmt": info["tile"]["fmt"],
                "siz": info["tile"]["siz"], "byte_count": byte_count, "load": load,
                "joinable": load["kind"] == "block" and load.get("loaded_bytes") == byte_count}
            wide[entry["hash"]] = record
            break
    groups = collections.defaultdict(dict)
    for record in wide.values():
        if record["joinable"]:
            key = (record["source_bank"], record["width"], record["fmt"], record["siz"])
            groups[key][record["source_address"]] = record
    consumed, joined = set(), []
    for group, addresses in groups.items():
        for address, first in sorted(addresses.items()):
            if first["image_id"] in consumed:
                continue
            chain, cursor = [first], address + first["byte_count"]
            while cursor in addresses and len(chain) < 256:
                candidate = addresses[cursor]
                if candidate["image_id"] in consumed:
                    break
                chain.append(candidate)
                cursor += candidate["byte_count"]
            if len(chain) < 2:
                continue
            width, height = first["width"], sum(item["height"] for item in chain)
            rgba, regions, y = bytearray(), [], 0
            for item in chain:
                entry = additions[item["image_id"]]
                # Read the already decoded PNG without reinterpreting TMEM.
                from PIL import Image
                with Image.open(entry["png"]) as image:
                    rgba.extend(image.convert("RGBA").tobytes())
                regions.append({"id": entry["image_id"], "rect": [0, y, width, y + item["height"]],
                    "hash_aliases": entry["hash_aliases"]})
                y += item["height"]
            ident = hashlib.sha256(struct.pack(">II", width, height) + rgba).hexdigest()
            path = target / "images" / f"{ident}.png"
            write_png(path, width, height, bytes(rgba))
            joined.append({"image_id": ident, "png": str(path.resolve()), "width": width, "height": height,
                "hash_aliases": [], "regions": regions, "source_images": [item["image_id"] for item in chain],
                "join_evidence": {"source_bank": group[0], "start_address": address, "end_address": cursor,
                    "method": "consecutive authored source bytes, identical render width/format, and exact per-strip byte counts",
                    "strips": chain}})
            consumed.update(item["image_id"] for item in chain)
    scrolls = collections.defaultdict(dict)
    for entry in report["entries"] + report["conditional_entries"]:
        if entry["hash"] not in all_by_hash:
            continue
        for source in entry["sources"]:
            match = re.search(r"_scroll(\d+)_frame(\d+)$", source["context"])
            info = source["metadata"]
            if not match or (info["width"], info["height"], info["tlut"], info["tile"]["fmt"], info["tile"]["siz"]) != (32, 64, "None", 0, 2):
                continue
            locations = source.get("load_locations", [{"bank": source["bank"], "offset": offset} for offset in source["loads"]])
            descriptors = [banks.get(loc["bank"], {}).get(loc["offset"]) for loc in locations]
            selected = [load for load in descriptors if load and load["segment"] in (9, 10)]
            if len(selected) != 1:
                continue
            image = all_by_hash[entry["hash"]]
            scrolls[selected[0]["segment"]][(int(match[1]), image["image_id"])] = image
    scroll_parents = []
    from PIL import Image
    for segment, samples in sorted(scrolls.items()):
        originals = [(phase, image) for (phase, _), image in samples.items() if phase == 0]
        if not originals:
            raise ValueError("scroll parent lacks the authored zero phase")
        with Image.open(originals[0][1]["png"]) as image:
            canonical = image.convert("RGBA").tobytes()
        regions = []
        for (phase, ident), entry in sorted(samples.items()):
            expected = canonical[phase * 32 * 4:] + canonical[:phase * 32 * 4]
            with Image.open(entry["png"]) as image:
                if image.convert("RGBA").tobytes() != expected:
                    raise ValueError(f"Twilight segment{segment} phase{phase} is not an exact cyclic row shift")
            regions.append({"id": ident, "rect": [0, phase, 32, phase + 64], "hash_aliases": entry["hash_aliases"]})
            consumed.add(ident)
        rgba = canonical * 2
        ident = hashlib.sha256(struct.pack(">II", 32, 128) + rgba).hexdigest()
        path = target / "images" / f"{ident}.png"
        write_png(path, 32, 128, rgba)
        scroll_parents.append({"image_id": ident, "png": str(path.resolve()), "width": 32, "height": 128,
            "hash_aliases": [], "regions": regions, "source_images": sorted({region["id"] for region in regions}),
            "periodic_y": 64,
            "periodic_output_requirement": "After enhancement, copy the generated first64 source rows into the second half before extracting every region; scale row counts by the integer upscale factor.",
            "join_evidence": {"method": "all source phases match exact cyclic RGBA row shifts of one64-row parent",
                "source_bank": "mio0_23", "source_segment": segment,
                "source_address": 0x0E00F478 if segment == 9 else 0x0E011480,
                "period_rows": 64, "phases": sorted({key[0] for key in samples}), "pixel_relationship_verified": True}})
    images = joined + scroll_parents + [entry for ident, entry in additions.items() if ident not in consumed]
    result = {"schema_version": 1, "images": images, "base_inventory": str(base_inventory.resolve()),
        "base_hash_alias_updates": [entry for ident, entry in merged.items() if ident in base_ids],
        "summary": {"unique_addon_source_images": len(additions), "joined_images": len(joined),
            "strips_joined": sum(len(entry["regions"]) for entry in joined), "generation_images": len(images),
            "scroll_phase_images_replaced": sum(len(entry["regions"]) for entry in scroll_parents),
            "cyclic_scroll_parents": len(scroll_parents), "replaces_base_images": len(consumed & base_ids),
            "remaining_width_512_or_larger": sum(entry["width"] >= 512 for entry in images)},
        "wide_source_layout": list(wide.values()), "static_report": str((output / "report.json").resolve())}
    (target / "inventory.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", type=Path, default=ROOT / "reference/wr64-decomp/baserom.us.rev1.z64")
    parser.add_argument("--inventory", type=Path, default=ROOT / "build/texture-qa/rom-inventory.json")
    parser.add_argument("--runtime", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--base-inventory", type=Path, help="also emit a merged addon excluding these already generated images")
    args = parser.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to(ROOT) or not output.relative_to(ROOT).parts[0].startswith("build"):
        parser.error("output must be under an ignored build*/ directory in this checkout")
    rom = args.rom.read_bytes()
    if hashlib.sha1(rom).hexdigest() != ROM_SHA1:
        parser.error("requires USA Rev A big-endian ROM " + ROM_SHA1)
    inv = json.loads(args.inventory.read_text())
    if inv["rom_sha1"] != ROM_SHA1:
        parser.error("inventory ROM does not match the input")
    result = extract(rom, inv, args.runtime, output)
    print(json.dumps(result["summary"], sort_keys=True))
    if args.base_inventory:
        addon = build_addon(result, inv, args.runtime, output, args.base_inventory)
        print(json.dumps(addon["summary"], sort_keys=True))


if __name__ == "__main__":
    main()
