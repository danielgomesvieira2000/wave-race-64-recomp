"""Exercise gameplay feedback independently from the renderer and controller."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class HapticsMixerTests(unittest.TestCase):
    def test_gameplay_feedback(self):
        compiler = shutil.which('clang++') or shutil.which('c++')
        if not compiler:
            self.skipTest('C++ compiler required')
        with tempfile.TemporaryDirectory(prefix='wr64-haptics-mixer-') as directory:
            binary = Path(directory) / 'mixer'
            subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'include'), str(ROOT / 'src/haptics_mixer.cpp'),
                            str(ROOT / 'src/haptics_observation.cpp'),
                            str(ROOT / 'tools/tests/haptics_mixer_test.cpp'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
