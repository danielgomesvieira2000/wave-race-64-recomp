import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from check_linux_abi import check_glibc_requirements, check_binary_abi

class LinuxAbiCheckTests(unittest.TestCase):
    def test_legacy_three_component_versions_do_not_raise_the_floor(self):
        text = 'Name: GLIBC_2.2.5\nName: GLIBC_2.38\nName: GLIBCXX_3.4.32\n'
        self.assertEqual(check_glibc_requirements(text), (2, 38))

    def test_new_glibc_requirement_prevents_packaging(self):
        with self.assertRaisesRegex(ValueError, '2.43'):
            check_glibc_requirements('Name: GLIBC_2.43\nName: GLIBC_2.38\n')

    def test_private_glibc_abi_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'PRIVATE'):
            check_glibc_requirements('Name: GLIBC_PRIVATE\n')

    def test_non_elf_input_fails_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / 'not-an-executable'
            path.write_text('not ELF')
            with self.assertRaises(ValueError):
                check_binary_abi(path)

if __name__ == '__main__':
    unittest.main()
