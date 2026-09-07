#!/usr/bin/env python3
"""Apply the frontend bridge and bounded replacement-texture mip sampling.

The owning frontend context retains its lifetime. Wave Race uses this accessor
only from the runtime renderer thread, before submitting display lists.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET = ROOT / "lib/RecompFrontend/recompui/include/recompui/renderer.h"
SAMPLER_TARGET = ROOT / "lib/RT64/src/shaders/TextureSampler.hlsli"
ANCHOR = "            float get_resolution_scale() const override;"
ACCESSOR = "            RT64::Application* application() const { return app.get(); }"
SAMPLER_ORIGINAL = """            float mipBias = -0.25f;
            mip = 0.5 * log2(ddMax) + mipBias;
            float maxMip = float(gpuTile.textureDimensions.z - 1);
            RDPMipLevels[0] = min(floor(mip), maxMip);
            RDPMipLevels[1] = min(floor(mip) + 1, maxMip);"""
SAMPLER_REPLACEMENT = """            float mipBias = -0.25f;
            float maxMip = float(gpuTile.textureDimensions.z - 1);
            // Magnification must stay on mip zero. Bound the floating-point
            // LOD before converting to unsigned levels, and keep zero UV
            // derivatives finite so the trilinear weight cannot become NaN.
            mip = clamp(0.5 * log2(max(ddMax, 1.0e-8f)) + mipBias, 0.0f, maxMip);
            RDPMipLevels[0] = uint(floor(mip));
            RDPMipLevels[1] = min(RDPMipLevels[0] + 1U, uint(maxMip));"""


def patch_text(source: str) -> str:
    if ACCESSOR in source:
        return source
    if source.count(ANCHOR) != 1:
        raise ValueError("renderer accessor anchor changed; inspect the pinned frontend")
    return source.replace(ANCHOR, ANCHOR + "\n\n" +
                          "            // Borrowed application; the renderer context owns its lifetime.\n" + ACCESSOR, 1)


def patch_sampler_text(source: str) -> str:
    if source.count(SAMPLER_REPLACEMENT) == 1 and SAMPLER_ORIGINAL not in source:
        return source
    if source.count(SAMPLER_ORIGINAL) != 1 or SAMPLER_REPLACEMENT in source:
        raise ValueError("custom mip sampler anchor changed; inspect the pinned RT64")
    return source.replace(SAMPLER_ORIGINAL, SAMPLER_REPLACEMENT, 1)


def main() -> None:
    prepared = []
    for target, patch, label in [(TARGET, patch_text, "RecompFrontend texture accessor"),
                                  (SAMPLER_TARGET, patch_sampler_text, "RT64 bounded texture mip sampler")]:
        if not target.is_file():
            raise SystemExit(f"Missing {target}; run git submodule update --init --recursive")
        original = target.read_text()
        try:
            prepared.append((target, original, patch(original), label))
        except ValueError as error:
            raise SystemExit(f"{target}: {error}") from error
    # Validate both anchors before changing either dependency.
    for target, original, patched, label in prepared:
        if patched != original:
            target.write_text(patched)
            if target == SAMPLER_TARGET:
                # This pinned RT64 build does not track shader include files
                # in its custom-command dependencies. Invalidate the sole
                # consuming entry point so an incremental build recompiles all
                # RasterPS variants with the corrected sampler.
                (target.parent / "RasterPS.hlsl").touch()
            print(f"{label}: patched")
        else:
            print(f"{label}: already patched")


if __name__ == "__main__":
    main()
