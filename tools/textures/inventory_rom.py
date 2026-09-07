#!/usr/bin/env python3
"""Inventory USA Rev A authored asset banks and static texture-load candidates.

This is an offline coverage aid, not a claim that all materials were rendered.
It reads the ROM without changing it and writes only the requested JSON report.
RDP state inherited from parent display lists, runtime palettes, animation and
relocation still require the runtime texture dump to establish replacement keys.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[2]
ROM_SHA1 = "508dfc2d4caa42b6f6de5263d0aed5e44ac7966a"
COURSES = ["Dolphin Park", "Sunny Beach", "Sunset Bay", "Marine Fortress",
           "Drake Lake", "Port Blue", "Twilight City", "Glacier Coast", "Southern Island"]
FORMATS = {0: "RGBA", 1: "YUV", 2: "CI", 3: "IA", 4: "I"}


def u32(data, offset):
    if offset < 0 or offset + 4 > len(data):
        raise ValueError(f"32-bit read outside input at {offset:#x}")
    return struct.unpack_from(">I", data, offset)[0]


def decode_mio0(rom, start, limit):
    """Decode within the next stream boundary; reject every truncated channel."""
    data = memoryview(rom)[start:limit]
    if len(data) < 16 or bytes(data[:4]) != b"MIO0":
        raise ValueError("missing MIO0 header")
    size, compressed, raw = struct.unpack_from(">III", data, 4)
    if not 0 < size <= 16 * 1024 * 1024:
        raise ValueError("invalid decoded length")
    if not 16 <= compressed <= raw <= len(data):
        raise ValueError("invalid channel offsets")
    layout_end, compressed_end = compressed, raw
    layout, mask, bits = 16, 0, 0
    output = bytearray()
    while len(output) < size:
        if mask == 0:
            if layout >= layout_end:
                raise ValueError("truncated layout channel")
            bits = data[layout]
            layout += 1
            mask = 128
        if bits & mask:
            if raw >= len(data):
                raise ValueError("truncated literal channel")
            output.append(data[raw])
            raw += 1
        else:
            if compressed + 2 > compressed_end:
                raise ValueError("truncated backreference channel")
            code = struct.unpack_from(">H", data, compressed)[0]
            compressed += 2
            length, distance = (code >> 12) + 3, (code & 4095) + 1
            if distance > len(output):
                raise ValueError("backreference precedes decoded data")
            if len(output) + length > size:
                raise ValueError("backreference exceeds decoded length")
            for _ in range(length):
                output.append(output[-distance])
        mask >>= 1
    return bytes(output), max(layout, compressed, raw)


def texture_loads(data):
    """Recognize actual RDP load command sequences, not arbitrary image bytes.

    F5/E6/F3 (or F4/F0) follows FD, allowing intervening sync commands. Render
    dimensions are recorded only for a complete immediate LoadTextureBlock
    macro. Other loads retain their honest unresolved inherited-state status.
    """
    results = []
    sync = {0xE6, 0xE7, 0xE8}
    for offset in range(0, len(data) - 7, 8):
        image, address = struct.unpack_from(">II", data, offset)
        if image >> 24 != 0xFD or (image >> 21 & 7) not in FORMATS or address >> 24 > 15:
            continue
        cursor, load_tile, load = offset + 8, None, None
        while cursor + 8 <= len(data) and cursor <= offset + 64:
            w0, w1 = struct.unpack_from(">II", data, cursor)
            opcode = w0 >> 24
            if opcode in sync:
                pass
            elif opcode == 0xF5 and load_tile is None:
                load_tile = (w0, w1)
            elif opcode in (0xF0, 0xF3, 0xF4) and load_tile is not None:
                load = (cursor, w0, w1)
                break
            else:
                break
            cursor += 8
        if load is None:
            continue
        tile0, tile1 = load_tile
        if (tile0 >> 21 & 7) not in FORMATS:
            continue
        position, load0, load1 = load
        opcode = load0 >> 24
        if (tile1 >> 24 & 7) != (load1 >> 24 & 7):
            continue
        descriptor = {
            "command_offset": offset, "image_address": address,
            "segment": address >> 24, "segment_offset": address & 0xFFFFFF,
            "image_format": FORMATS[image >> 21 & 7], "image_bits": 4 << (image >> 19 & 3),
            "image_width_field": (image & 4095) + 1,
            "load_format": FORMATS[tile0 >> 21 & 7], "load_bits": 4 << (tile0 >> 19 & 3),
            "load_tile": tile1 >> 24 & 7, "tmem_word": tile0 & 511,
            "kind": {0xF0: "palette", 0xF3: "block", 0xF4: "tile"}[opcode],
        }
        if opcode == 0xF0:
            descriptor["palette_entries"] = (load1 >> 14 & 1023) + 1
            descriptor["loaded_bytes"] = descriptor["palette_entries"] * 2
        elif opcode == 0xF3:
            texels = (load1 >> 12 & 4095) + 1
            descriptor.update(load_texels=texels, dxt=load1 & 4095,
                              loaded_bytes=(texels * descriptor["load_bits"] + 7) // 8)
        else:
            descriptor["load_rectangle_fixed_10_2"] = [load0 >> 12 & 4095, load0 & 4095,
                                                         load1 >> 12 & 4095, load1 & 4095]
        # Immediate post-load render tile and size is the authoritative macro
        # layout. Keep the remaining loads for later control-flow/state analysis.
        cursor, render_tile = position + 8, None
        while cursor + 8 <= len(data) and cursor <= position + 40:
            w0, w1 = struct.unpack_from(">II", data, cursor)
            op = w0 >> 24
            if op in sync:
                pass
            elif op == 0xF5 and render_tile is None:
                render_tile = (w0, w1)
            elif op == 0xF2 and render_tile is not None:
                t0, t1 = render_tile
                width = ((w1 >> 12 & 4095) - (w0 >> 12 & 4095)) // 4 + 1
                height = ((w1 & 4095) - (w0 & 4095)) // 4 + 1
                if (t1 >> 24 & 7) == (w1 >> 24 & 7) and (t0 >> 21 & 7) in FORMATS and width > 0 and height > 0:
                    descriptor["render_tile"] = {"width": width, "height": height,
                        "format": FORMATS[t0 >> 21 & 7], "bits": 4 << (t0 >> 19 & 3),
                        "palette": t1 >> 20 & 15, "line_words": t0 >> 9 & 511,
                        "tmem_word": t0 & 511, "tile": t1 >> 24 & 7}
                break
            else:
                break
            cursor += 8
        results.append(descriptor)
    return results


def inventory(rom, named):
    starts = [match.start() for match in re.finditer(b"MIO0", rom)]
    banks = []
    for index, start in enumerate(starts):
        limit = starts[index + 1] if index + 1 < len(starts) else len(rom)
        decoded, consumed = decode_mio0(rom, start, limit)
        banks.append({"id": f"mio0_{named[start]}" if start in named else f"mio0_at_{start:06X}",
            "named_index": named.get(start), "rom_start": start,
            "compressed_end": start + consumed, "next_stream_boundary": limit,
            "decoded_bytes": len(decoded), "decoded_sha256": hashlib.sha256(decoded).hexdigest(),
            "loads": texture_loads(decoded)})
    by_address = {bank["rom_start"]: bank["id"] for bank in banks}

    def read(address):
        return u32(rom, address - 0x80045800)

    def dma_list(address):
        entries = []
        for index in range(64):
            start, end, flag, offset = struct.unpack_from(">IIII", rom, address - 0x80045800 + index * 16)
            if flag == 0:
                return entries
            if not 0 <= start < end <= len(rom) or not 1 <= flag <= 10:
                raise ValueError(f"invalid DMA entry at {address + index * 16:#x}")
            entries.append({"bank": by_address.get(start), "rom_start": start, "rom_end": end,
                            "flag": flag, "destination_offset": offset})
        raise ValueError("unterminated DMA list")

    courses = []
    tables = {"one_player_geometry": 0x800DC4F0, "two_player_geometry": 0x800DC514,
              "one_player_raw_data": 0x800DC698, "two_player_raw_data": 0x800DC6BC,
              "one_player_surface_layers": 0x800DC0E4, "two_player_surface_layers": 0x800DC108,
              "shared_course_assets": 0x800DC18C}
    for index, name in enumerate(COURSES):
        item = {"id": index, "name": name}
        for label, table in tables.items():
            item[label] = dma_list(read(table + index * 4))
        object_ids = struct.unpack_from(">hh", rom, 0x95D68 + index * 4)
        item["course_object_banks"] = [by_address[read(0x800DB4A8 + obj * 16)] for obj in object_ids if obj >= 0]
        courses.append(item)
    riders = []
    for index in range(16):
        start = read(0x800DABD0 + index * 16)
        riders.append({"table_index": index, "bank": by_address[start],
                       "rider": ["R. Hayami", "D. Mariner", "A. Stewart", "M. Jeter"][index % 4],
                       "alternate": bool(index & 4), "additional_model_variant": bool(index & 8)})

    raw_regions = []
    for name, start, end in [("uncompressed_setup_assets", 0xF6090, 0xF7440),
                              ("uncompressed_assets", 0xF7440, 0x1AE660),
                              ("uncompressed_course_data", 0x2A4ED0, 0x2E5FB0)]:
        data = rom[start:end]
        raw_regions.append({"id": name, "rom_start": start, "rom_end": end,
                            "bytes": len(data), "loads": texture_loads(data)})
    all_loads = [load for bank in banks + raw_regions for load in bank["loads"]]
    return {"schema_version": 1, "rom_sha1": hashlib.sha1(rom).hexdigest(),
        "rom_bytes": len(rom), "coverage_kind": "offline authored-bank and static-load inventory",
        "summary": {"mio0_streams": len(banks), "named_mio0_banks": len(named),
            "decoded_bytes": sum(bank["decoded_bytes"] for bank in banks),
            "static_load_candidates": len(all_loads),
            "load_kinds": dict(collections.Counter(load["kind"] for load in all_loads)),
            "loads_with_immediate_render_dimensions": sum("render_tile" in load for load in all_loads)},
        "limitations": [
            "Load candidates are command-pattern matches, not unique texture or material counts.",
            "Dimensions are emitted only when an immediate render-tile/size pair provides them.",
            "Inherited display-list state, runtime-generated commands and runtime palettes remain unresolved.",
            "No RT64 replacement hashes are inferred; runtime dump hashes are a separate coverage set.",
            "The two-player Dolphin Park table entry aliases Sunny Beach; this does not establish a playable mode.",
        ],
        "banks": banks, "raw_regions": raw_regions, "courses": courses, "rider_bank_variants": riders,
        "source_tables": {key: hex(value) for key, value in tables.items()},
        "capture_matrix": {
            "course_scenes": list(range(9)), "players": [1, 2],
            "difficulty": ["normal", "hard", "expert"],
            "race_modes": ["championship", "time_trials", "stunt", "two_player_vs"],
            "riders": [0, 1, 2, 3], "rider_outfits": ["normal", "alternate"],
            "extra_scenes": ["boot", "title", "attract_demo", "main_menu", "rider_select", "course_select",
                "course_overview", "all_options_pages", "pause", "all_results_pages", "award_ceremony"],
            "motion": ["all three laps", "camera near/far", "underwater and above water",
                       "jumps and tricks", "missed buoys", "crash and recovery", "animated course objects"],
            "acceptance": "Track new runtime hashes per fixture and compare source-bank/material coverage; a plateau alone is not proof of completeness."
        }}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", type=Path, default=ROOT / "reference/wr64-decomp/baserom.us.rev1.z64")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    rom = args.rom.read_bytes()
    if hashlib.sha1(rom).hexdigest() != ROM_SHA1:
        parser.error("asset tables require the USA Rev A big-endian ROM (SHA1 " + ROM_SHA1 + ")")
    yaml = (ROOT / "reference/wr64-decomp/waverace64.us.rev1.yaml").read_text()
    named = {int(address, 16): int(index) for address, index in
             re.findall(r"\[(0x[0-9A-F]+), bin, mio0_(\d+)\]", yaml)}
    result = inventory(rom, named)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result["summary"], sort_keys=True))


if __name__ == "__main__":
    main()
