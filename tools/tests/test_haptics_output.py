"""Compile and exercise the SDL backend with actual virtual-controller callbacks."""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class HapticsOutputTests(unittest.TestCase):
    def test_virtual_controllers(self):
        config = os.environ.get('SDL2_CONFIG')
        if not config:
            bundled = ROOT / 'build-macos-deps/install/bin/sdl2-config'
            config = str(bundled) if bundled.exists() else shutil.which('sdl2-config')
        compiler = shutil.which('clang++') or shutil.which('c++')
        if not config or not compiler:
            self.skipTest('SDL2 development runtime and C++ compiler are required')
        flags = shlex.split(subprocess.check_output([config, '--cflags', '--libs'], text=True))
        with tempfile.TemporaryDirectory(prefix='wr64-haptics-output-') as directory:
            binary = Path(directory) / 'haptics-output-test'
            command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'include'),
                       str(ROOT / 'src/haptics_output.cpp'), str(ROOT / 'tools/tests/haptics_output_test.cpp')]
            if sys.platform == 'darwin':
                prefix = subprocess.check_output([config, '--prefix'], text=True).strip()
                command += ['-Wl,-rpath,' + str(Path(prefix) / 'lib')]
            subprocess.run(command + flags + ['-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
