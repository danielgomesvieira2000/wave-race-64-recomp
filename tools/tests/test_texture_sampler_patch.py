"""Exercise the patched mip selector through the project's actual Metal toolchain."""
import importlib.util
import json
import os
from pathlib import Path
import platform
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('texture_patch', ROOT / 'tools/patch_texture_packs.py')
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)


class TextureSamplerPatchTests(unittest.TestCase):
    def test_idempotent_and_rejects_changed_anchor(self):
        source = 'prefix\n' + patch.SAMPLER_ORIGINAL + '\nsuffix'
        corrected = patch.patch_sampler_text(source)
        self.assertEqual(patch.patch_sampler_text(corrected), corrected)
        self.assertTrue(corrected.startswith('prefix\n'))
        self.assertTrue(corrected.endswith('\nsuffix'))
        with self.assertRaises(ValueError):
            patch.patch_sampler_text(source.replace('log2(ddMax)', 'log2(ddMax + 1)'))

    @unittest.skipUnless(platform.system() == 'Darwin' and platform.machine() == 'arm64', 'requires the local Apple Silicon Metal toolchain')
    def test_finite_bounded_gpu_lods_and_unchanged_minification(self):
        directory = ROOT / 'build/texture-qa/lod-regression'
        directory.mkdir(parents=True, exist_ok=True)
        dxc = ROOT / 'lib/RT64/src/contrib/dxc/bin/arm64/dxc-macos'
        cross = ROOT / 'build/bin/spirv_cross_msl'
        if not dxc.is_file() or not cross.is_file():
            self.skipTest('build the repository DXC and SPIR-V-to-Metal tools first')
        live_sampler = patch.SAMPLER_TARGET.read_text()
        self.assertIn(patch.SAMPLER_REPLACEMENT, live_sampler)
        environment = dict(os.environ, DYLD_LIBRARY_PATH=str(ROOT / 'lib/RT64/src/contrib/dxc/lib/arm64'))
        executable = directory / 'lod-gpu'
        subprocess.run(['clang++', '-std=c++17', '-fobjc-arc', '-framework', 'Foundation', '-framework', 'Metal',
                        str(ROOT / 'tools/tests/texture_lod_gpu.mm'), '-o', str(executable)], check=True)
        measurements = {}
        for name, body in [('before', patch.SAMPLER_ORIGINAL), ('after', patch.SAMPLER_REPLACEMENT)]:
            source = '''struct Tile { float3 textureDimensions; };
StructuredBuffer<float> inputs : register(t0);
RWStructuredBuffer<float4> outputs : register(u1);
[numthreads(1,1,1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
 float ddMax = inputs[tid.x]; Tile gpuTile; gpuTile.textureDimensions = float3(256, 256, 9);
 float mip; uint RDPMipLevels[2];
''' + body + '\n outputs[tid.x] = float4(RDPMipLevels[0], RDPMipLevels[1], frac(mip), mip);\n}\n'
            hlsl, spv, metal = [directory / f'{name}.{suffix}' for suffix in ('hlsl', 'spv', 'metal')]
            hlsl.write_text(source)
            subprocess.run([str(dxc), '-spirv', '-fspv-target-env=vulkan1.0', '-T', 'cs_6_3', '-E', 'CSMain',
                            '-fvk-use-dx-layout', str(hlsl), '-Fo', str(spv)], check=True, env=environment)
            subprocess.run([str(cross), str(spv), str(metal)], check=True)
            measurements[name] = json.loads(subprocess.check_output([str(executable), str(metal)], text=True))
        (directory / 'measurements.json').write_text(json.dumps(measurements, indent=2) + '\n')
        after = measurements['after']
        for index, row in enumerate(after):
            self.assertTrue(all(value is not None for value in row), (index, row))
            self.assertTrue(0 <= row[0] <= row[1] <= 8, (index, row))
            self.assertTrue(0 <= row[2] < 1, (index, row))
            self.assertTrue(0 <= row[3] <= 8, (index, row))
        for row in after[:5]:
            self.assertEqual(row, [0, 1, 0, 0])
        for index in range(5, 9):
            self.assertEqual(after[index], measurements['before'][index])
        self.assertEqual(after[-1], [8, 8, 0, 8])


if __name__ == '__main__':
    unittest.main()
