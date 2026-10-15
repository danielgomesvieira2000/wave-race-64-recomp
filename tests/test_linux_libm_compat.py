"""ROM-free regression for the Linux x86-64 libm link contract."""
import pathlib
import os
import sys
import platform
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from check_linux_abi import check_binary_abi

SOURCE = r'''#include <math.h>
int main(void) {
    volatile float zero = 0.0f, one = 1.0f, nine = 9.0f;
    return !(sqrtf(nine) == 3.0f && asinf(zero) == 0.0f &&
             acosf(one) == 0.0f && atan2f(zero, one) == 0.0f);
}
'''

@unittest.skipUnless(platform.system() == 'Linux' and platform.machine() == 'x86_64'
                     and platform.libc_ver()[0] == 'glibc',
                     'Linux x86-64 ELF ABI regression')
class LinuxLibmCompatTests(unittest.TestCase):
    def test_cmake_policy_reaches_c_and_cpp_in_a_dependency(self):
        if not all(shutil.which(t) for t in ('cmake', 'cc', 'c++', 'objdump')):
            self.skipTest('C/C++ build tools are required')
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            (root / 'dependency').mkdir()
            (root / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.20)\nproject(probe LANGUAGES C CXX)\n'
                f'include("{ROOT}/cmake/linux_libm_compat.cmake")\n'
                'add_subdirectory(dependency)\n')
            (root / 'dependency/CMakeLists.txt').write_text(
                'add_executable(probe_c probe.c)\ntarget_link_libraries(probe_c PRIVATE m)\n'
                'add_executable(probe_cpp probe.cpp)\ntarget_link_libraries(probe_cpp PRIVATE m)\n'
                'target_compile_options(probe_c PRIVATE -fno-builtin)\n'
                'target_compile_options(probe_cpp PRIVATE -fno-builtin)\n'
                'add_executable(no_math no_math.c)\n')
            (root / 'dependency/probe.c').write_text(SOURCE)
            (root / 'dependency/probe.cpp').write_text(SOURCE)
            # A forced header must not freeze libc features before the source
            # defines its own feature-test macros.
            (root / 'dependency/no_math.c').write_text(
                '#define _GNU_SOURCE\n#include <string.h>\n'
                'int main(void) { return strcmp(strdupa("ok"), "ok"); }\n')
            subprocess.run(['cmake', '-S', str(root), '-B', str(root / 'build')], check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            subprocess.run(['cmake', '--build', str(root / 'build')], check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            for name in ('probe_c', 'probe_cpp'):
                binary = root / 'build/dependency' / name
                symbols = subprocess.check_output(['objdump', '-T', str(binary)], text=True)
                for function in ('atan2f', 'asinf', 'acosf', 'sqrtf'):
                    self.assertRegex(symbols, rf'(?m)^.*GLIBC_2\.2\.5\)?\s+{function}$')
                check_binary_abi(binary)
                subprocess.run([str(binary)], check=True)
            subprocess.run([str(root / 'build/dependency/no_math')], check=True)
            native_version = subprocess.check_output(['getconf', 'GNU_LIBC_VERSION'], text=True).split()[1]
            if tuple(map(int, native_version.split('.'))) >= (2, 43):
                # Reproduce the release failure with real native libm imports,
                # rather than a mocked readelf report, on a glibc 2.43 builder.
                unpinned = root / 'unpinned'
                subprocess.run([os.environ.get('CC', 'cc'), '-fno-builtin',
                                str(root / 'dependency/probe.c'), '-lm', '-o', str(unpinned)], check=True)
                with self.assertRaisesRegex(ValueError, '2.43'):
                    check_binary_abi(unpinned)

if __name__ == '__main__':
    unittest.main()
