#!/usr/bin/env python3
"""Distribution failure cases for required music and HD texture staging."""

import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import wave
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import bundled_assets as assets


def write_json(path, value):
    path.write_text(json.dumps(value) + "\n", encoding="utf-8")


def chunk(name, data):
    return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name + data))


def fixture(root):
    music = root / "music"
    music.mkdir(parents=True)
    track = music / "test.wav"
    with wave.open(str(track), "wb") as audio:
        audio.setparams((2, 2, 48000, 48000, "NONE", "not compressed"))
        audio.writeframes(b"\0" * 192000)
    (music / "CREDITS.md").write_text("Test recording\n")
    loop = {"start_seconds": 0, "end_seconds": 1, "crossfade_seconds": 0, "gain_db": 0}
    write_json(music / "loops.json", {"test": loop})
    write_json(music / "manifest.json", {
        "schema_version": 1, "credits_file": "CREDITS.md", "loops_file": "loops.json",
        "loops_sha256": assets.digest(music / "loops.json"),
        "tracks": [{"id": "test", "output": "test.wav", "output_bytes": track.stat().st_size,
                    "output_frames": 48000, "output_sha256": assets.digest(track), "loop": loop}],
    })
    pack = root / assets.PACK
    (pack / "textures").mkdir(parents=True)
    (pack / "CREDITS.md").write_text("Test textures\n")
    png = pack / "textures/ui.png"
    png.write_bytes(b"\x89PNG\r\n\x1a\n"
                    + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
                    + chunk(b"IDAT", zlib.compress(b"\0\xff\xff\xff\xff")) + chunk(b"IEND", b""))
    dds = pack / "textures/world.dds"
    header = bytearray(128)
    header[:4] = b"DDS "
    struct.pack_into("<7I", header, 4, 124, 0x2100F, 2, 2, 8, 0, 2)
    struct.pack_into("<8I", header, 76, 32, 0x41, 0, 32, 255, 65280, 16711680, 4278190080)
    struct.pack_into("<I", header, 108, 0x401008)
    dds.write_bytes(header + b"\xff" * 20)
    write_json(pack / "rt64.json", {
        "configuration": {"hashVersion": 5},
        "textures": [{"hashes": {"rt64": "0123456789abcdef"}, "path": "textures/ui.png"},
                     {"hashes": {"rt64": "fedcba9876543210"}, "path": "textures/world.dds"}],
    })
    files = [{"path": p.relative_to(pack).as_posix(), "bytes": p.stat().st_size,
              "sha256": assets.digest(p), "width": w, "height": h, "mip_levels": m}
             for p, w, h, m in ((png, 1, 1, 1), (dds, 2, 2, 2))]
    write_json(pack / "manifest.json", {
        "schema_version": 1, "pack_id": "nano-banana-2", "hash_version": 5,
        "mapping_count": 2, "image_count": 2, "database_sha256": assets.digest(pack / "rt64.json"),
        "total_image_bytes": sum(f["bytes"] for f in files), "files": files, "credits_file": "CREDITS.md",
    })


class BundledAssetsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        fixture(self.source)
        self.pack = self.source / assets.PACK

    def change_database(self, callback):
        database = assets.read_json(self.pack, "rt64.json")
        callback(database)
        write_json(self.pack / "rt64.json", database)
        manifest = assets.read_json(self.pack, "manifest.json")
        manifest["database_sha256"] = assets.digest(self.pack / "rt64.json")
        write_json(self.pack / "manifest.json", manifest)

    def test_valid_pack_and_refresh_preserve_other_assets(self):
        destination = self.root / "app/assets"
        (destination / "icons").mkdir(parents=True)
        (destination / "icons/menu.svg").write_text("menu")
        other = destination / "textures/custom"
        other.mkdir(parents=True)
        (other / "rt64.json").write_text("custom")
        report = assets.stage(self.source, destination)
        self.assertEqual(report["textures"]["mappings"], 2)
        staged = destination / assets.PACK
        (staged / "textures/obsolete.png").write_bytes(b"obsolete")
        (staged / "textures/ui.png").write_bytes(b"broken")
        assets.stage(self.source, destination)
        self.assertEqual(assets.fingerprint(destination), assets.fingerprint(self.source))
        self.assertFalse((staged / "textures/obsolete.png").exists())
        self.assertEqual((destination / "icons/menu.svg").read_text(), "menu")
        self.assertEqual((other / "rt64.json").read_text(), "custom")

    def test_missing_pack_fails(self):
        (self.pack / "manifest.json").unlink()
        with self.assertRaisesRegex(ValueError, "missing or empty"):
            assets.validate(self.source)

    def test_missing_image_fails(self):
        (self.pack / "textures/ui.png").unlink()
        with self.assertRaisesRegex(ValueError, "missing or empty"):
            assets.validate(self.source)

    def test_corrupt_image_and_music_fail(self):
        for relative in (assets.PACK / "textures/ui.png", Path("music/test.wav")):
            with self.subTest(relative=relative):
                path = self.source / relative
                original = path.read_bytes()
                path.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
                with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                    assets.validate(self.source)
                path.write_bytes(original)

    def test_parent_path_is_rejected_even_with_updated_database_digest(self):
        self.change_database(lambda db: db["textures"][0].update(path="textures/../../outside.png"))
        with self.assertRaisesRegex(ValueError, "must be relative"):
            assets.validate(self.source)

    def test_duplicate_runtime_hash_is_rejected(self):
        self.change_database(lambda db: db["textures"][1].update(hashes=db["textures"][0]["hashes"]))
        with self.assertRaisesRegex(ValueError, "duplicate RT64"):
            assets.validate(self.source)

    def test_stale_valid_app_is_rejected_by_reference(self):
        destination = self.root / "app/assets"
        assets.stage(self.source, destination)
        (destination / assets.PACK / "CREDITS.md").write_text("Older release credits\n")
        with self.assertRaisesRegex(ValueError, "differ from the source checkout"):
            assets.validate(destination, self.source)

    def test_unmanifested_capture_is_rejected(self):
        (self.pack / "capture.tmem").write_bytes(b"raw capture")
        with self.assertRaisesRegex(ValueError, "Unmanifested"):
            assets.validate(self.source)

    def test_stage_refuses_escaping_directory_link(self):
        destination = self.root / "app/assets"
        (destination / assets.PACK).mkdir(parents=True)
        outside = self.root / "outside"
        outside.mkdir()
        (destination / assets.PACK / "textures").symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "escapes its pack"):
            assets.stage(self.source, destination)
        self.assertEqual(list(outside.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
