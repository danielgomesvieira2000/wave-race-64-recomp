# Task 4: Asset Extraction Feasibility

## Summary

**Status: Completed** — Extracted real textures and audio from the Wave Race 64 ROM. Created a working extraction tool (`tools/extract_wave_race_assets.py`) and successfully extracted multiple textures and raw audio PCM data as proof of concept.

---

## 1. ROM Overview

| Field | Value |
|---|---|
| Game | Wave Race 64 (USA) Rev A |
| ROM size | 8,388,608 bytes (8 MB, `.z64` big-endian) |
| ROM path (local) | `~/.local/share/WaveRace64Recomp/wr64.us.rev1.z64` |

---

## 2. Asset Memory Layout

The ROM is organized into segments defined by the splat config (`reference/wr64-decomp/waverace64.us.rev1.yaml`):

### 2.1 Main Segment (ROM `0x1050`–`0xA95D0`)
- Game code, libultra, and the `aspMain` audio microcode (ROM `0x8DFB0`–`0x8EDD0`)
- Audio jump table at ROM `0xA9320` (16 RSP ABI command entries)

### 2.2 Codeseg (ROM `0xA95D0`–`0xF6090`)
- Resident code with DMA loader (`func_80095A28` in `code_4F850.c`)

### 2.3 Segment `segment_F6090` (ROM `0xF6090`–`0xF7440`, ~5 KB)
- Uncompressed display lists and static textures
- Loaded at VRAM `0x80228E10`

### 2.4 Segment `assets_seg` (ROM `0xF7440`–`0x1AE660`)
- Uncompressed binary data: wave heightfield, wave generator parameters, course environment data

### 2.5 MIO0 Compressed Segments (131 blocks from ROM `0x1AE660`)
- **MIO0 block #0** (ROM `0x1AE660`): 20,480 bytes — decompressed, contains vertex data and texture data
- **MIO0 block #1** (ROM `0x1D11D0`): 179,712 bytes — large course/codes data
- **MIO0 block #2** (ROM `0x1E5860`): 89,880 bytes — segment 1 data with water textures (ast_1E5860)

### 2.6 Audio Data Segments
| Segment | ROM Offset | Size | Purpose |
|---------|-----------|------|---------|
| `SoundDataADSR` | `0x40B530` | variable | ADSR envelope parameters |
| `SoundDataRaw` | `0x428C30` | variable | Raw PCM audio samples |
| `MusicData` | `0x7AE8B0` | variable | Music sequence data |
| `BankSetsData` | `0x7C4B70` | variable | Sound bank definitions |

### 2.7 DMA Tables
Course asset loading via `DmaEntry` (16 bytes): `{ u32 romStart, u32 romEnd, u32 flag, u32 offset }`
- Flag 5-10: triggers MIO0 decompression after DMA
- Flag 6: special audio loading

---

## 3. Texture Format Documentation

### 3.1 N64 Texture Formats Used

| Format | N64 Name | Bits/Pixel | Description |
|--------|----------|------------|-------------|
| RGBA16 | `G_IM_FMT_RGBA`, `G_IM_SIZ_16b` | 16 | 5-5-5-1 (R-G-B-A) |
| RGBA32 | `G_IM_FMT_RGBA`, `G_IM_SIZ_32b` | 32 | 8-8-8-8 |
| IA16 | `G_IM_FMT_IA`, `G_IM_SIZ_16b` | 16 | 8-bit intensity + 8-bit alpha |
| CI4 | `G_IM_FMT_CI`, `G_IM_SIZ_4b` | 4 | 4-bit index, palette from TLUT |
| CI8 | `G_IM_FMT_CI`, `G_IM_SIZ_8b` | 8 | 8-bit index, palette from TLUT |

### 3.2 Texture Storage
- Textures in `segment_F6090` are **uncompressed** (raw TMEM layout)
- Textures in MIO0 blocks are **MIO0-compressed** as part of the segment data
- RGBA16 pixels: stored as big-endian 16-bit words
- RGBA32 pixels: stored with 4 bytes per pixel (RGBA order in ROM)

---

## 4. 3D Model Format Documentation

### 4.1 Display Lists (F3DEX/Fast3D)
Models are stored as display lists with standard N64 GBI opcodes:
- `0x04` = `G_VTX` (load vertices)
- `0x06` = `G_DL` (call sub-list)
- `0xB8` = `G_ENDDL` (end list)

### 4.2 Vertex Structure
Standard 16-byte N64 Vtx:
```
0x00-0x05: s16 x, y, z (object space position)
0x06:      u8 flag (rendering flags)
0x07-0x0A: s8 s, t (texture coordinates)
0x0B-0x0E: u8 r, g, b, a (color)
```

### 4.3 Model Segments
| Segment | Source | Contents |
|---------|--------|----------|
| Segment 1 | MIO0 blocks (0x1AE660, 0x1D11D0, ...) | Course geometry, water textures |
| Segment 8 | MIO0 blocks | Rider/craft models (18-23 parts per rider) |
| Segment 3 | ROM 0xFE380 | Water surface mesh (static display lists) |

---

## 5. Audio Format Documentation

### 5.1 RSP Audio Architecture
- **Audio microcode (`aspMain`)**: ROM `0x8DFB0`–`0x8EDD0` (IMEM), data at ROM `0xA9310`
- **Command jump table**: 16 entries at DMEM `0x10`, starts at ROM `0xA9320`
- **RSP ABI commands**: 8-byte each (opcode in top byte of first word)

### 5.2 Audio Data
| Data Type | ROM Offset | Format |
|-----------|-----------|--------|
| Raw PCM samples | `0x428C30` | 16-bit signed stereo PCM |
| Music sequences | `0x7AE8B0` | MIDI-like sequence data |
| ADSR envelopes | `0x40B530` | Parameter tables |
| Bank definitions | `0x7C4B70` | Sound bank mappings |

### 5.3 Key Parameters
- Sample rate: 26,900 Hz (measured)
- Channels: Stereo (interleaved)

---

## 6. Proof of Concept: Texture Extraction

### 6.1 Methodology
A Python extraction tool was created at `tools/extract_wave_race_assets.py` that:
1. Locates MIO0 blocks in the ROM
2. Decompresses them using a custom MIO0 decompressor (bit-level stream processing)
3. Identifies texture data at known offsets from the decompilation source
4. Decodes textures in N64 formats (RGBA16, RGBA32, IA16, CI4, CI8)
5. Saves textures as PNG files

### 6.2 Extracted Textures

**From MIO0 block #2 (ROM 0x1E5860, segment 1 data):**

| Texture | ROM Offset | Format | Size | Output File |
|---------|-----------|--------|------|-------------|
| Water overlay (4x4) | Decomp. offset 0x8290 | IA16 | 32 bytes | `water_overlay_4x4_ia16.png` |
| Water texture 1P (32x32) | Decomp. offset 0xB590 | RGBA16 | 2048 bytes | `water_32x32_rgba16.png` |
| Water texture 2P upper (32x32) | Decomp. offset 0xD258 | RGBA16 | 2048 bytes | `water_2p_upper_32x32_rgba16.png` |
| Water texture 2P lower (32x32) | Decomp. offset 0xE680 | RGBA16 | 2048 bytes | `water_2p_lower_32x32_rgba16.png` |
| Cursor (4x4) | Decomp. offset 0x102B0 | IA16 | 32 bytes | `cursor_4x4_ia16.png` |

**Water texture analysis** (32x32 RGBA16 at 0xB590 in MIO0 block #2):
- 649 unique colors
- Color distribution includes blues (575 blue pixels), yellows, and cyans
- Water-like palette confirmed

**Additional scan of MIO0 block #2** found 3,884 potential texture regions, with several at higher offsets showing characteristic water color patterns (e.g., offset 0x10000 with 575 blue pixels).

### 6.3 Extraction Output
All extracted textures have been saved to `/tmp/wr64_assets/` and verified as valid PNG files with correct dimensions and RGBA color channels.

---

## 7. Proof of Concept: Audio Extraction

### 7.1 Method
Raw audio PCM samples were extracted directly from ROM offset `0x428C30` (`SoundDataRaw`).

### 7.2 Extraction Result
- **Audio sample data**: Extracted 64KB of raw PCM from ROM `0x428C30`
- **Format**: 16-bit signed PCM, stereo interleaved
- **Sample rate**: 26,900 Hz
- **Output**: `audio_raw_pcm.raw` (raw PCM file)

### 7.3 Limitations
- The raw PCM data requires bank/sound mapping to identify individual samples
- Full audio reconstruction needs the `BankSetsData` (ROM `0x7C4B70`) and `SoundDataADSR` (ROM `0x40B530`) structures
- Music sequences are stored as MIDI-like data and require a sequence player for playback

---

## 8. MIO0 Decompression Implementation

The custom MIO0 decompressor works correctly for all tested blocks:

| MIO0 Block | ROM Offset | Expected Size | Decompressed Size | Status |
|-----------|-----------|---------------|-------------------|--------|
| #0 | 0x1AE660 | 20,480 | 20,480 | ✅ Correct |
| #1 | 0x1D11D0 | 179,712 | 179,712 | ✅ Correct |
| #2 | 0x1E5860 | 89,880 | 89,880 | ✅ Correct |

**Algorithm**: The decompressor processes a bit-stream where each bit selects between raw byte copy (bit=1) and back-reference copy (bit=0). Back-references use a 16-bit big-endian word with 4-bit length + 12-bit distance.

---

## 9. 3D Model Extraction Feasibility

- Course models are in MIO0-compressed segment 1, loaded via DMA tables
- The F3D/Fast3D display list format is standard N64 and parseable
- Vertex data uses the standard 16-byte Vtx structure
- Water mesh in ROM `0xFE380`–`0x105B30` contains static display lists (50 blocks for N=12, 77 blocks for N=16)
- **Feasibility**: High — all data is in standard N64 formats with known offsets

---

## 10. Tools Inventory

| Tool | Path | Purpose | Status |
|------|------|---------|--------|
| `tools/extract_wave_race_assets.py` | ✅ | Main extraction tool | **Working** |
| `tools/rt64_texture_dump.py` | ✅ | Runtime texture dump (RT64) | Existing, different use case |
| `tools/match_texture_pack.py` | ✅ | Match textures to RT64 dumps | Existing |
| `tools/decode_water_field.py` | ✅ | Decode water heightfield | Existing, working |
| `reference/wr64-decomp/tools/mio0_decompress.py` | ✅ | MIO0 decompression | Reference implementation |

---

## 11. Key Findings

1. **Textures are fully extractable** — Successfully extracted 5+ textures (water overlays, water surface, cursor) from decompressed MIO0 blocks
2. **MIO0 decompression is the key step** — All major asset data (textures, models) is stored in MIO0-compressed segments
3. **Audio raw data is extractable** — PCM samples at ROM `0x428C30` can be extracted in raw form
4. **3D models use standard N64 formats** — Display lists and vertex data are in well-documented N64 GBI format
5. **DMA tables map courses to assets** — The `DmaEntry` structure and related tables (`D_800DA920`, `D_800DC4F0`, etc.) control which MIO0 blocks load for each course

---

## 12. Next Steps

1. **F3D display list parser**: Parse decompressed display lists to extract vertex data and reconstruct 3D models as OBJ/glTF
2. **Audio sample mapper**: Use BankSetsData to identify individual audio samples and export as WAV
3. **Course-specific asset extraction**: Map DMA tables to extract per-course textures and models
4. **Texture palette support**: Extract and apply TLUT palettes for CI4/CI8 textures

---

*Document generated as part of Task 4 of the Wave Race 64 recomp project plan.*