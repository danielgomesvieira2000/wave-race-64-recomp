#!/usr/bin/env python3
"""Exercise static replay against the actual RT64 C++ TMEM loader."""
import ctypes
from pathlib import Path
import platform
import random
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/textures"))
import extract_static as static


class LoaderReferenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        directory = Path(cls.temporary.name)
        cpp = (ROOT / "lib/RT64/src/hle/rt64_rdp.cpp").read_text()
        start = cpp.index("    template<bool RGBA32 = false, bool TLUT = false>")
        end = cpp.index("    void RDP::loadTileOperation", start)
        loader = cpp[start:end]
        source = """
#include <cstdint>
#include <cassert>
#define __forceinline inline
#define RDP_TMEM_BYTES 4096
#define RDP_TMEM_MASK16 2047
#define RDP_TMEM_MASK8 4095
""" + loader + """
extern "C" void reference_load(uint8_t *dst, const uint8_t *src, const uint32_t *v) {
 bool rgba=v[0],block=v[1],tlut=v[2];
#define LOAD(R,B,T) loadToTMEMCommon<R,B,T>(dst,src,v[3],v[4],v[5],v[6],v[7],v[8],v[9])
 if(rgba) { if(block) LOAD(true,true,false); else if(tlut) LOAD(true,false,true); else LOAD(true,false,false); }
 else { if(block) LOAD(false,true,false); else if(tlut) LOAD(false,false,true); else LOAD(false,false,false); }
}
"""
        source_path, library = directory / "loader.cpp", directory / "loader.so"
        source_path.write_text(source)
        command = ["c++", "-std=c++17", "-O2"]
        command += ["-dynamiclib"] if platform.system() == "Darwin" else ["-shared", "-fPIC"]
        subprocess.run(command + [str(source_path), "-o", str(library)], check=True, capture_output=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.reference = cls.lib.reference_load
        cls.reference.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
        cls.hash = static.Hasher(directory / "hash")

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def check_loader(self, tile, image, opcode):
        rng = random.Random(449)
        source = bytes(rng.randrange(256) for _ in range(32768))
        memory = bytearray([0xBA] * 4096)
        known = bytearray(4096)
        static.load_tmem(memory, known, source, tile, image, opcode)
        block, tlut = opcode == 0xF3, opcode == 0xF0
        stride = image["width"] << image["siz"] >> 1
        if block:
            source_start = (tile["uls"] << image["siz"] >> 1) + stride * tile["ult"]
            rows = 1
            words = ((tile["lrs"] - tile["uls"]) >> (4 - tile["siz"])) + 1
        else:
            source_start = ((tile["uls"] >> 2) << image["siz"] >> 1) + stride * (tile["ult"] >> 2)
            rows = 1 + (tile["lrt"] >> 2) - (tile["ult"] >> 2)
            width = (tile["lrs"] >> 2) - (tile["uls"] >> 2)
            words = width + 1 if tlut else (width >> (4 - tile["siz"])) + 1
        params = (ctypes.c_uint32 * 10)(tile["fmt"] == 0 and tile["siz"] == 3,
            block, tlut, source_start, stride, tile["tmem"] << 3,
            tile["line"] << (5 if tlut else 3), words, rows, tile["lrt"] if block else 0)
        reference = ctypes.create_string_buffer(bytes([0xBA]) * 4096, 4096)
        # RT64's host RDRAM is little-endian 32-bit words; offline ROM data is big-endian.
        host = b"".join(source[i:i + 4][::-1] for i in range(0, len(source), 4))
        self.reference(reference, host, params)
        self.assertEqual(bytes(memory), reference.raw)
        self.assertTrue(any(known))

    def test_exact_loader_matches(self):
        cases = 0
        for fmt, siz in ((0, 2), (0, 3), (2, 1), (3, 0), (3, 2), (4, 1)):
            for tmem in (0, 31, 500):
                for opcode in (0xF3, 0xF4):
                    tile = dict.fromkeys(static.TILE_FIELDS, 0)
                    tile.update(fmt=fmt, siz=siz, tmem=tmem)
                    if opcode == 0xF3:
                        tile.update(line=0, uls=0, ult=0, lrs=511, lrt=0x180)
                    else:
                        tile.update(line=8, uls=12, ult=8, lrs=136, lrt=28)
                    with self.subTest(fmt=fmt, siz=siz, tmem=tmem, opcode=opcode):
                        self.check_loader(tile, {"width": 64, "siz": siz}, opcode)
                    cases += 1
        for start, entries in ((256, 256), (288, 16), (400, 32)):
            tile = dict.fromkeys(static.TILE_FIELDS, 0)
            tile.update(tmem=start, siz=0, lrs=(entries - 1) * 4)
            self.check_loader(tile, {"width": 1, "siz": 2}, 0xF0)
            cases += 1
        self.assertEqual(cases, 39)

    def test_failed_load_is_atomic(self):
        tile = dict.fromkeys(static.TILE_FIELDS, 0)
        tile.update(siz=2, lrs=63)
        memory, known = bytearray([23] * 4096), bytearray([1] * 4096)
        with self.assertRaises(static.Unresolved):
            static.load_tmem(memory, known, bytes(8), tile, {"width": 1, "siz": 2}, 0xF3)
        self.assertEqual(memory, bytes([23] * 4096))
        self.assertEqual(known, bytes([1] * 4096))

    def test_hash_rejects_only_bytes_actually_read(self):
        tile = dict.fromkeys(static.TILE_FIELDS, 0)
        tile.update(fmt=0, siz=2, line=4)
        info = {"width": 8, "height": 2, "tile": tile, "tlut": "None"}
        memory, known = bytes(4096), bytearray([1] * 4096)
        expected = self.hash(memory, known, info)
        known[300] = 0
        self.assertEqual(self.hash(memory, known, info), expected)
        known[36] = 0  # odd-row word swap is part of the actual hash implementation.
        with self.assertRaises(static.Unresolved):
            self.hash(memory, known, info)

    def test_missing_palette_is_never_invented(self):
        tile = dict.fromkeys(static.TILE_FIELDS, 0)
        tile.update(fmt=2, siz=1, line=1)
        info = {"width": 8, "height": 1, "tile": tile, "tlut": "RGBA16"}
        memory = bytes(4096)
        known = bytearray([1] * 2048 + [0] * 2048)
        with self.assertRaises(static.Unresolved):
            self.hash(memory, known, info)
        known[2048:2056] = bytes([1] * 8)
        self.assertEqual(len(self.hash(memory, known, info)), 16)

    def test_mask_dimensions_match_renderer(self):
        tile = dict.fromkeys(static.TILE_FIELDS, 0)
        tile.update(lrs=31 * 4, lrt=63 * 4, masks=4, maskt=5, cms=2, cmt=2)
        self.assertEqual(static.sample_dimensions(tile), (16, 32))
        tile.update(cms=0, cmt=0, masks=6, maskt=7)
        self.assertEqual(static.sample_dimensions(tile), (64, 128))

    def test_graph_preserves_parent_load_and_child_tile_state(self):
        def pack(commands):
            return b"".join(struct.pack(">II", *command) for command in commands)
        parent = pack([(0xBB000001, 0xFFFFFFFF), (0xFD100000, 0x08000100),
            (0xF5100000, 0x07000000), (0xE6000000, 0),
            (0xF3000000, 0x0701F400), (0x06000000, 0x08000080), (0xB8000000, 0)])
        child = pack([(0xBA000E02, 0), (0xF5100400, 0),
            (0xF2000000, 0x0001C00C), (0xBF000000, 0x0000050A), (0xB8000000, 0)])
        data = parent.ljust(128, b"\0") + child
        data = data.ljust(256, b"\0") + bytes(range(64))
        segments = {8: (data, 0)}
        graph, locations, calls = static.flatten_graph(data, 0, segments, {id(data): "fixture"})
        snapshots, errors = static.replay_region(graph, 0, len(graph), segments)
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["target_offset"], 128)
        self.assertEqual(len(snapshots), 1)
        self.assertEqual(errors, [])
        snap = snapshots[0]
        self.assertEqual((snap["width"], snap["height"], snap["tlut"]), (8, 4, "None"))
        self.assertEqual(len(self.hash(snap["tmem"], snap["known"], snap)), 16)
        self.assertEqual(locations[snap["command_offset"] // 8]["offset"], 152)

    def test_graph_rejects_cycles_and_unknown_targets(self):
        cycle = struct.pack(">IIII", 0x06000000, 0x08000000, 0xB8000000, 0)
        with self.assertRaisesRegex(static.Unresolved, "cycle"):
            static.flatten_graph(cycle, 0, {8: (cycle, 0)}, {})
        with self.assertRaisesRegex(static.Unresolved, "segment"):
            static.flatten_graph(cycle, 0, {}, {})
        endless = bytes(64)
        with self.assertRaisesRegex(static.Unresolved, "command_limit"):
            static.flatten_graph(endless, 0, {}, {}, max_commands=4)

    def test_vertex_coordinates_do_not_invent_rdp_mode(self):
        # Real preceding vertex words in Marine Fortress/Twilight banks once
        # looked like RDP opcodes to an opcode-byte-only backwards scan.
        for words in ((0xEF6BFFC4, 0xEB6A0000), (0xF47FFFC4, 0xF9980000),
                      (0xFFFFFFFF, 0xFFFFFFFF)):
            with self.subTest(words=words):
                self.assertFalse(static.valid_command(struct.pack(">II", *words), 0))
        self.assertTrue(static.valid_command(struct.pack(">II", 0xBA000E02, 0x8000), 0))

    def test_tail_branch_does_not_execute_caller_remainder(self):
        data = struct.pack(">IIIIIIIIII", 0x06010000, 0x08000018,
            0xFD100000, 0x08001000, 0xB8000000, 0,
            0xBA000E02, 0, 0xB8000000, 0)
        graph, locations, calls = static.flatten_graph(data, 0, {8: (data, 0)}, {})
        self.assertEqual(graph, struct.pack(">II", 0xBA000E02, 0))
        self.assertTrue(calls[0]["tail"])


if __name__ == "__main__":
    unittest.main()
