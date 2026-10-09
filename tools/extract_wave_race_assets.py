"""Extract Wave Race 64 game assets from the ROM.

This tool extracts textures, audio samples, and other binary assets directly
from the Wave Race 64 ROM file. It does not require the game to be running.

Usage:
    python tools/extract_wave_race_assets.py <rom_path> <output_dir> [options]

Options:
    --texture <offset> <size> <format> <width> <height> <output>
        Extract a single texture from a ROM offset.
        Format: rgba16, rgba32, ia16, ci4, ci8, i8, ia4

    --list-mio0
        List all MIO0 blocks found in the ROM.

    --extract-mio0 <index> <output_file>
        Decompress and extract a specific MIO0 block by index.

    --extract-audio
        Extract raw audio samples from their ROM location.

    --all
        Extract all known textures and audio samples.

Examples:
    # Extract all known assets
    python tools/extract_wave_race_assets.py baserom.z64 output/ --all
"""
import argparse
import os
import struct
import sys
from pathlib import Path

# N64 image format constants
G_IM_FMT_RGBA = 0
G_IM_FMT_CI = 2
G_IM_FMT_IA = 3
G_IM_FMT_I = 4

G_IM_SIZ_4b = 0
G_IM_SIZ_8b = 1
G_IM_SIZ_16b = 2
G_IM_SIZ_32b = 3


def read_u32_be(data, offset):
    """Read a big-endian 32-bit integer."""
    return struct.unpack_from('>I', data, offset)[0]


def decompress_mio0(data):
    """Decompress an MIO0-compressed block.
    
    MIO0 format:
        0x00-0x03: "MIO0" magic
        0x04-0x07: Uncompressed size
        0x08-0x0B: Offset to compressed data (from start of block)
        0x0C-0x0F: Offset to raw/uncompressed data (from start of block)
    
    Decompression uses a bit-packed stream (starting at 0x10):
        Each bit determines copy mode:
        bit=0: copy one byte from raw data section
        bit=1: copy length bytes from earlier output (back-reference)
    
    Note: The raw_offset in the header points to where raw data starts.
    If the actual block in ROM spans beyond raw_offset + raw_data_size,
    the decompressor will naturally stop once uncompressed_size is reached.
    """
    if data[:4] != b'MIO0':
        raise ValueError("Not a valid MIO0 file")
    
    uncompressed_size = read_u32_be(data, 4)
    compressed_offset = read_u32_be(data, 8)
    raw_offset = read_u32_be(data, 12)
    
    output = bytearray()
    bitstream = data[16:compressed_offset]
    bit_pos = 0
    comp_data_pos = compressed_offset
    raw_data_pos = raw_offset
    
    while len(output) < uncompressed_size:
        byte_idx = bit_pos // 8
        if byte_idx >= len(bitstream):
            break
        
        bit = (bitstream[byte_idx] >> (7 - (bit_pos % 8))) & 1
        bit_pos += 1
        
        if bit == 0:
            # Compressed: read 2-byte big-endian backreference
            if comp_data_pos + 2 > len(data):
                break
            backref = read_u32_be(data, comp_data_pos) & 0xFFFF
            comp_data_pos += 2
            
            length = ((backref >> 12) & 0xF) + 3
            distance = (backref & 0x0FFF) + 1
            
            for _ in range(length):
                if len(output) >= distance:
                    output.append(output[-distance])
                else:
                    # Back-reference before start - pad with zeros
                    output.append(0)
        else:
            # Raw byte
            if raw_data_pos < len(data):
                output.append(data[raw_data_pos])
                raw_data_pos += 1
            else:
                break
    
    return bytes(output[:uncompressed_size])


def find_mio0_blocks(rom_data):
    """Scan the ROM for all MIO0 block headers.
    
    Returns a list of dicts with rom_offset, uncompressed_size, 
    compressed_offset, raw_offset, and total_size.
    """
    blocks = []
    pos = 0
    while pos < len(rom_data) - 16:
        if rom_data[pos:pos+4] == b'MIO0':
            uncompressed_size = read_u32_be(rom_data, pos + 4)
            compressed_offset = read_u32_be(rom_data, pos + 8)
            raw_offset = read_u32_be(rom_data, pos + 12)
            
            # Validate: offsets should be reasonable
            # raw_offset can be 0 in some edge cases (raw data at end of block)
            if (uncompressed_size > 0 and uncompressed_size < 10 * 1024 * 1024
                    and compressed_offset >= 16 and raw_offset >= compressed_offset
                    and raw_offset <= 0x200000):
                # Total block size is from start to where raw data ends
                # The actual block might be larger (raw data extends beyond raw_offset)
                # but we can use compressed_offset + raw_offset as minimum
                blocks.append({
                    'rom_offset': pos,
                    'uncompressed_size': uncompressed_size,
                    'compressed_offset': compressed_offset,
                    'raw_offset': raw_offset,
                    'total_size': compressed_offset + raw_offset,
                })
        pos += 4
    return blocks


def reverse_word_endian(data):
    """Reverse each 4-byte word (N64 big-endian -> host little-endian for RGBA32)."""
    result = bytearray()
    for i in range(0, len(data) - 3, 4):
        result.extend(data[i:i+4][::-1])
    remainder = len(data) % 4
    if remainder:
        result.extend(data[-remainder:])
    return bytes(result)


def decode_rgba16(data, width, height):
    """Decode RGBA16 (5-5-5-1) texture data from N64 raw format."""
    pixels = []
    for i in range(0, len(data) - 1, 2):
        v = (data[i] << 8) | data[i+1]
        r = ((v >> 11) & 0x1F) * 255 // 31
        g = ((v >> 6) & 0x1F) * 255 // 31
        b = ((v >> 1) & 0x1F) * 255 // 31
        a = (v & 1) * 255
        pixels.append((r, g, b, a))
    return width, height, pixels


def decode_rgba32(data, width, height):
    """Decode RGBA32 (8-8-8-8) texture data from N64 raw format.
    
    On N64, RGBA32 is stored as RGBA in memory order but the 32-bit word
    is byte-swapped relative to host-endian. We need to handle this.
    """
    # N64 texture data for RGBA32 is stored with each pixel as:
    # byte0=byte0, byte1=byte1, byte2=byte2, byte3=byte3
    # where the 32-bit word is (R << 24 | G << 16 | B << 8 | A) in big-endian
    # When read on little-endian host, bytes are reversed
    
    # Actually for N64 textures: the raw data in ROM is in big-endian order
    # For RGBA32, each 4-byte pixel is stored as: R, G, B, A (big-endian)
    # When loaded into RDRAM, it becomes: A, B, G, R (little-endian)
    # For display as PNG, we need R, G, B, A order
    
    # The simplest approach: the data is in RGBA order when read MSB-first
    pixels = []
    for i in range(0, len(data) - 3, 4):
        # As stored in ROM (big-endian word): byte0, byte1, byte2, byte3
        # This maps to: R, G, B, A
        r = data[i]
        g = data[i+1]
        b = data[i+2]
        a = data[i+3]
        pixels.append((r, g, b, a))
    return width, height, pixels


def decode_ia16(data, width, height):
    """Decode IA16 (8-bit intensity + 8-bit alpha) texture data."""
    pixels = []
    for i in range(0, min(len(data), width * height * 2) - 1, 2):
        intensity = data[i]
        alpha = data[i+1]
        pixels.append((intensity, intensity, intensity, alpha))
    return width, height, pixels


def decode_ci4(data, width, height, palette):
    """Decode CI4 (4-bit color-indexed with palette) texture data."""
    pixels = []
    for i in range(0, len(data), 2):
        hi = (data[i] >> 4) & 0xF
        lo = data[i] & 0xF
        if hi < len(palette):
            pixels.append(palette[hi])
        if lo < len(palette):
            pixels.append(palette[lo])
    return width, height, pixels[:width * height]


def decode_ci8(data, width, height, palette):
    """Decode CI8 (8-bit color-indexed with palette) texture data."""
    pixels = []
    for i in range(min(len(data), width * height)):
        idx = data[i]
        if idx < len(palette):
            pixels.append(palette[idx])
    return width, height, pixels


def decode_palette(data):
    """Decode an RGBA16 palette (TLUT)."""
    palette = []
    for i in range(0, len(data) - 1, 2):
        v = (data[i] << 8) | data[i+1]
        r = ((v >> 11) & 0x1F) * 255 // 31
        g = ((v >> 6) & 0x1F) * 255 // 31
        b = ((v >> 1) & 0x1F) * 255 // 31
        a = (v & 1) * 255
        palette.append((r, g, b, a))
    return palette


def save_png(width, height, pixels, path):
    """Save pixels as a PNG file using PIL/Pillow."""
    try:
        from PIL import Image
        img = Image.new('RGBA', (width, height))
        for y in range(height):
            for x in range(width):
                idx = y * width + x
                if idx < len(pixels):
                    img.putpixel((x, y), pixels[idx])
        img.save(path)
        return True, path
    except ImportError:
        raw_path = str(path).replace('.png', '.raw')
        with open(raw_path, 'wb') as f:
            for p in pixels[:width * height]:
                f.write(bytes(p))
        return True, raw_path


def decode_and_save_texture(data, offset, size, fmt, width, height, output_path, palette_data=None):
    """Extract a texture from data and save it as PNG."""
    if offset + size > len(data):
        print(f"  Error: requested {size} bytes at 0x{offset:X} but data is only {len(data)} bytes")
        size = min(size, len(data) - offset)
    
    tex_data = data[offset:offset + size]
    
    if fmt == 'rgba16':
        w, h, pixels = decode_rgba16(tex_data, width, height)
    elif fmt == 'rgba32':
        w, h, pixels = decode_rgba32(tex_data, width, height)
    elif fmt == 'ia16':
        w, h, pixels = decode_ia16(tex_data, width, height)
    elif fmt == 'ci4':
        palette = decode_palette(palette_data) if palette_data else [(0,0,0,255)] * 16
        w, h, pixels = decode_ci4(tex_data, width, height, palette)
    elif fmt == 'ci8':
        palette = decode_palette(palette_data) if palette_data else [(0,0,0,255)] * 256
        w, h, pixels = decode_ci8(tex_data, width, height, palette)
    elif fmt == 'i8':
        w, h, pixels = width, height, [(b,b,b,b) for b in tex_data[:width*height]]
    else:
        print(f"  Unknown format: {fmt}")
        return None
    
    success, output = save_png(w, h, pixels, output_path)
    if success:
        print(f"  Extracted {fmt} texture: {w}x{h} -> {output}")
    return output


def main():
    parser = argparse.ArgumentParser(
        description="Extract Wave Race 64 assets from ROM",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    parser.add_argument('rom', help='Path to the Wave Race 64 ROM file')
    parser.add_argument('output', help='Output directory')
    parser.add_argument('--list-mio0', action='store_true', help='List all MIO0 blocks')
    parser.add_argument('--extract-mio0', nargs=2, metavar=('INDEX', 'OUTPUT'),
                        help='Decompress MIO0 block by index')
    parser.add_argument('--extract-audio', action='store_true', help='Extract raw audio samples')
    parser.add_argument('--all', action='store_true', help='Extract all known textures and audio')
    parser.add_argument('--scan-textures', action='store_true',
                        help='Scan a MIO0 block for potential textures')

    args = parser.parse_args()

    rom_path = Path(args.rom)
    if not rom_path.exists():
        print(f"Error: ROM file not found: {rom_path}")
        sys.exit(1)

    with open(rom_path, 'rb') as f:
        rom_data = f.read()

    print(f"ROM: {rom_path} ({len(rom_data)} bytes)")
    output_dir = Path(args.output)
    output_dir.mkdir(parents=True, exist_ok=True)

    # Find MIO0 blocks first
    blocks = find_mio0_blocks(rom_data)
    print(f"Found {len(blocks)} MIO0 blocks")

    if args.list_mio0:
        for i, b in enumerate(blocks):
            print(f"  #{i:3d}: ROM 0x{b['rom_offset']:08X}, "
                  f"usize={b['uncompressed_size']:7d} (0x{b['uncompressed_size']:06X})")

    if args.extract_mio0:
        index = int(args.extract_mio0[0])
        output_file = args.extract_mio0[1]
        if index >= len(blocks):
            print(f"Error: MIO0 block index {index} out of range")
            sys.exit(1)
        
        block = blocks[index]
        # Read the full block - need to handle the fact that the MIO0 block
        # extends beyond the header's calculated size
        next_pos = min(len(rom_data), block['rom_offset'] + 1)
        for j in range(index + 1, len(blocks)):
            if blocks[j]['rom_offset'] > block['rom_offset']:
                next_pos = blocks[j]['rom_offset']
                break
        else:
            next_pos = len(rom_data) - 1
        
        block_size = next_pos - block['rom_offset']
        block_data = rom_data[block['rom_offset']:block['rom_offset'] + block_size]
        
        print(f"Decompressing MIO0 block #{index} at ROM 0x{block['rom_offset']:X}...")
        try:
            decompressed = decompress_mio0(block_data)
            with open(output_file, 'wb') as f:
                f.write(decompressed)
            print(f"  Decompressed {len(decompressed)} bytes -> {output_file}")
        except Exception as e:
            print(f"  Error: {e}")

    if args.extract_audio:
        print("\n=== Extracting audio samples ===")
        
        # SoundDataRaw at ROM 0x428C30 - raw PCM samples
        audio_offset = 0x428C30
        # Extract in chunks of different sizes
        for size, name in [(0x10000, '4kb'), (0x4EF0, '20kb'), (0x100000, '256kb')]:
            if audio_offset + size <= len(rom_data):
                output = output_dir / f"audio_raw_{name}.raw"
                with open(output, 'wb') as f:
                    f.write(rom_data[audio_offset:audio_offset + size])
                print(f"  Raw PCM ({size} bytes) -> {output}")
        
        # MusicData at ROM 0x7AE8B0
        music_offset = 0x7AE8B0
        if music_offset < len(rom_data):
            output = output_dir / "music_data.bin"
            # Look for next MIO0 or segment boundary
            end = len(rom_data)
            for b in blocks:
                if b['rom_offset'] > music_offset:
                    end = b['rom_offset']
                    break
            size = min(0x50000, end - music_offset)
            with open(output, 'wb') as f:
                f.write(rom_data[music_offset:music_offset + size])
            print(f"  Music sequences ({size} bytes) -> {output}")

    if args.all:
        print("\n=== Texture extraction ===")
        
        # 1. Extract from segment_F6090 (ROM 0xF6090-0xF7440)
        seg_start = 0xF6090
        seg_end = 0xF7440
        seg_data = rom_data[seg_start:seg_end]
        print(f"\n  segment_F6090: {len(seg_data)} bytes (ROM 0x{seg_start:X}-0x{seg_end:X})")
        
        # This segment contains display lists and some textures
        # The water texture at offset 0x8290 in segment 1 is in MIO0 block #2

        # 2. Decompress MIO0 block #0 (ROM 0x1AE660 -> segment 1)
        # Size: 20480 bytes, first block in the MIO0_chunk segment
        print("\n  Decompressing MIO0 block #0 (ROM 0x1AE660)...")
        block = blocks[0]
        next_pos = blocks[1]['rom_offset'] if len(blocks) > 1 else len(rom_data)
        block_size = next_pos - block['rom_offset']
        block_data = rom_data[block['rom_offset']:block['rom_offset'] + block_size]
        try:
            decompressed_0 = decompress_mio0(block_data)
            mio0_0_out = output_dir / "mio0_block_0_decompressed.bin"
            with open(mio0_0_out, 'wb') as f:
                f.write(decompressed_0)
            print(f"    Decompressed {len(decompressed_0)} bytes -> {mio0_0_out}")
            
            # Scan for textures in the decompressed data
            print(f"    Scanning for textures...")
            tex_found = 0
            for off in range(0, len(decompressed_0) - 2048, 16):
                chunk = decompressed_0[off:off+2048]
                nz = sum(1 for b in chunk if b != 0)
                if nz > 1800:
                    colors = set()
                    for i in range(0, 2048, 2):
                        v = (chunk[i] << 8) | chunk[i+1]
                        r = ((v >> 11) & 0x1F) * 255 // 31
                        g = ((v >> 6) & 0x1F) * 255 // 31
                        b = ((v >> 1) & 0x1F) * 255 // 31
                        colors.add((r, g, b))
                    if len(colors) > 50:
                        output = output_dir / f"mio0_0_tex_{off:X}_rgba16_32x32.png"
                        decode_and_save_texture(decompressed_0, off, 2048, 'rgba16', 32, 32, str(output))
                        tex_found += 1
                        if tex_found >= 15:
                            break
            print(f"    Found {tex_found} potential textures")
        except Exception as e:
            print(f"    Error: {e}")

        # 3. Decompress MIO0 block #2 (ROM 0x1E5860 -> ast data)
        print("\n  Decompressing MIO0 block #2 (ROM 0x1E5860)...")
        block = blocks[2]
        next_pos = blocks[3]['rom_offset'] if len(blocks) > 3 else len(rom_data)
        block_size = next_pos - block['rom_offset']
        block_data = rom_data[block['rom_offset']:block['rom_offset'] + block_size]
        try:
            decompressed_2 = decompress_mio0(block_data)
            mio0_2_out = output_dir / "mio0_block_2_decompressed.bin"
            with open(mio0_2_out, 'wb') as f:
                f.write(decompressed_2)
            print(f"    Decompressed {len(decompressed_2)} bytes -> {mio0_2_out}")
            
            # Extract water texture at offset 0x8290 (IA16 4x4)
            print(f"\n    Extracting water overlay textures...")
            decode_and_save_texture(decompressed_2, 0x8290, 32, 'ia16', 4, 4,
                                   str(output_dir / "water_overlay_4x4_ia16.png"))
            decode_and_save_texture(decompressed_2, 0x82F0, 32, 'ia16', 4, 4,
                                   str(output_dir / "water_overlay_82F0_4x4_ia16.png"))
            
            # Extract 32x32 RGBA16 water texture at 0xB590
            print(f"    Extracting 32x32 water textures...")
            for off, label in [(0xB590, 'water_1p'), (0xD258, 'water_2p_upper'), (0xE680, 'water_2p_lower')]:
                if off + 2048 <= len(decompressed_2):
                    output = output_dir / f"{label}_32x32_rgba16.png"
                    decode_and_save_texture(decompressed_2, off, 2048, 'rgba16', 32, 32, str(output))
            
            # Scan for more textures
            print(f"    Scanning for additional textures...")
            tex_found = 0
            for off in range(0, len(decompressed_2) - 2048, 16):
                chunk = decompressed_2[off:off+2048]
                nz = sum(1 for b in chunk if b != 0)
                if nz > 1800:
                    colors = set()
                    for i in range(0, 2048, 2):
                        v = (chunk[i] << 8) | chunk[i+1]
                        r = ((v >> 11) & 0x1F) * 255 // 31
                        g = ((v >> 6) & 0x1F) * 255 // 31
                        b = ((v >> 1) & 0x1F) * 255 // 31
                        colors.add((r, g, b))
                    if len(colors) > 50:
                        output = output_dir / f"mio0_2_tex_{off:X}_rgba16_32x32.png"
                        decode_and_save_texture(decompressed_2, off, 2048, 'rgba16', 32, 32, str(output))
                        tex_found += 1
                        if tex_found >= 15:
                            break
            print(f"    Found {tex_found} potential textures")
        except Exception as e:
            print(f"    Error: {e}")

        # 4. Raw audio extraction
        print("\n=== Audio extraction ===")
        audio_offset = 0x428C30
        output = output_dir / "audio_raw_pcm.raw"
        with open(output, 'wb') as f:
            f.write(rom_data[audio_offset:audio_offset + 0x10000])
        print(f"  Raw PCM audio samples -> {output}")

    if not args.list_mio0 and not args.extract_mio0 and not args.extract_audio and not args.all:
        # Default: extract key textures and audio
        print("\n=== Default extraction ===")
        
        # Extract from MIO0 block #2 (has water textures)
        block = blocks[2]
        next_pos = len(rom_data)
        for b in blocks[3:]:
            if b['rom_offset'] > block['rom_offset']:
                next_pos = b['rom_offset']
                break
        block_size = next_pos - block['rom_offset']
        block_data = rom_data[block['rom_offset']:block['rom_offset'] + block_size]
        
        decompressed = decompress_mio0(block_data)
        out = output_dir / "mio0_block_2_decompressed.bin"
        with open(out, 'wb') as f:
            f.write(decompressed)
        print(f"  Decompressed MIO0 #2 ({len(decompressed)} bytes) -> {out}")
        
        # Water overlay texture (4x4 IA16)
        decode_and_save_texture(decompressed, 0x8290, 32, 'ia16', 4, 4,
                               str(output_dir / "water_overlay_4x4_ia16.png"))
        
        # Water texture (32x32 RGBA16)
        if len(decompressed) > 0xB590 + 2048:
            decode_and_save_texture(decompressed, 0xB590, 2048, 'rgba16', 32, 32,
                                   str(output_dir / "water_32x32_rgba16.png"))
        
        # Audio
        audio_out = output_dir / "audio_raw_pcm.raw"
        with open(audio_out, 'wb') as f:
            f.write(rom_data[0x428C30:0x428C30 + 0x10000])
        print(f"  Audio PCM (64KB) -> {audio_out}")

    print("\nDone.")


if __name__ == '__main__':
    main()