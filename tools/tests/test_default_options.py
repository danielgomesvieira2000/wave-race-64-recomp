"""Exercise shipped option declarations with librecomp's real config loader."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / 'lib/N64ModernRuntime'


class DefaultOptionsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang++') or shutil.which('c++')
        if not compiler:
            raise unittest.SkipTest('C++ compiler unavailable')
        cls.temp = tempfile.TemporaryDirectory(prefix='wr64-default-options-')
        cls.directory = Path(cls.temp.name)
        source = (ROOT / 'src/frontend.cpp').read_text()
        declarations = []
        # Compile the actual frontend declarations, rather than a second list
        # of defaults that could pass while the production menu still differs.
        for name in ['texture_quality', 'water_quality', 'water_style',
                     'water_ripples', 'water_spray', 'music_replacements']:
            match = re.search(r'(?:graphics|water|sound)\.add_enum_option\("' + name + r'".*?\);', source, re.S)
            if not match:
                raise AssertionError(f'Cannot locate shipped option {name}')
            declarations.append(re.sub(r'^(graphics|water|sound)\.', 'config.', match.group()))
        code = r'''
#include <cassert>
#include <fstream>
#include <iostream>
#include <filesystem>
#include "librecomp/config.hpp"
#include "librecomp/game.hpp"
#include "wr64/textures.h"
namespace fs = std::filesystem;
fs::path settings;
namespace recomp {
fs::path get_config_path() { return settings; }
const Version& get_project_version() { static const Version version{0,0,0};return version; }
}
void add_options(recomp::config::Config& config) {
''' + '\n'.join(declarations) + r'''
}
uint32_t value(const recomp::config::Config& config, const char* key) {
    return std::get<uint32_t>(config.get_option_value(key));
}
int main(int argc, char** argv) {
    assert(argc==3); settings=argv[2];fs::create_directories(settings);
    const std::string test=argv[1];
    if (test=="fresh" || test=="saved" || test=="missing-key") {
        if(test=="saved") std::ofstream(settings/"appearance.json") << R"({"texture_quality":"original","water_quality":"original","water_style":"classic","water_ripples":"soft","water_spray":"off","music_replacements":"original"})";
        if(test=="missing-key") std::ofstream(settings/"appearance.json") << R"({"water_quality":"original","water_spray":"off"})";
        recomp::config::Config config("Appearance","appearance",true);add_options(config);
        bool callback_spray=true;
        config.add_option_change_callback("water_spray",[&](auto v,auto,auto context){
            if(context!=recomp::config::OptionChangeContext::Temporary)callback_spray=std::get<uint32_t>(v)!=0;
        });
        assert(config.load_config());
        assert(value(config,"texture_quality")==uint32_t(test=="saved"?0:1));
        assert(value(config,"water_quality")==uint32_t(test=="fresh"?2:0));
        assert(value(config,"water_style")==uint32_t(test=="saved"?1:0));
        assert(value(config,"music_replacements")==uint32_t(test=="saved"?0:1));
        assert(value(config,"water_spray")==uint32_t(test=="fresh"?1:0));
        assert(callback_spray==(test=="fresh"));
    } else if(test=="paths") {
        const auto bundled=settings/"Game.app/Contents/Resources/assets/textures/nano-banana-2";
        fs::create_directories(bundled);std::ofstream(bundled/"rt64.json")<<"{}";
        const auto installed=settings/"textures/nano-banana-2";
        using wr64::textures::resolve_pack_path;
        assert(resolve_pack_path(settings,bundled)==bundled);
        fs::create_directories(installed);
        assert(resolve_pack_path(settings,bundled)==installed);
        const auto explicit_pack=settings/"selected-pack.zip";
        assert(resolve_pack_path(settings,bundled,explicit_pack)==explicit_pack);
        fs::remove_all(installed);std::ofstream(installed)<<"archive placeholder";
        assert(resolve_pack_path(settings,bundled)==installed);
        fs::remove(installed);
        const auto windows_bundle=settings/"windows/assets/textures/nano-banana-2";
        fs::create_directories(windows_bundle);
        assert(resolve_pack_path(settings,windows_bundle)==windows_bundle);
        fs::remove_all(windows_bundle);
        assert(resolve_pack_path(settings,windows_bundle)==windows_bundle);
        // Discovery never creates an installed directory or mutates a pack.
        assert(!fs::exists(installed));
    } else return 2;
    std::cout<<"PASS "<<test<<"\n";
}
'''
        cpp = cls.directory / 'default_options_test.cpp'
        cpp.write_text(code)
        # CMake normally generates this export decoration header. No miniz
        # functions are called by the config loader exercised here.
        (cls.directory / 'miniz_export.h').write_text('#pragma once\n#define MINIZ_EXPORT\n')
        cls.binary = cls.directory / 'default_options_test'
        includes = [cls.directory, ROOT / 'include', RUNTIME / 'librecomp/include',
                    RUNTIME / 'librecomp/include/librecomp', RUNTIME / 'thirdparty',
                    RUNTIME / 'thirdparty/sse2neon',
                    RUNTIME / 'thirdparty/concurrentqueue', RUNTIME / 'thirdparty/miniz',
                    RUNTIME / 'ultramodern/include',
                    RUNTIME / 'N64Recomp/include']
        command = [compiler, '-std=c++20', '-O0', str(cpp)]
        command += [str(RUNTIME / 'librecomp/src' / name) for name in ['config.cpp', 'config_option.cpp', 'files.cpp']]
        command += [flag for directory in includes for flag in ['-I', str(directory)]]
        result = subprocess.run(command + ['-o', str(cls.binary)], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, case):
        subprocess.run([str(self.binary), case, str(self.directory / case)], check=True)

    def test_fresh_defaults(self):
        self.run_case('fresh')

    def test_saved_explicit_preferences(self):
        self.run_case('saved')

    def test_missing_new_options_use_defaults(self):
        self.run_case('missing-key')

    def test_pack_path_precedence(self):
        self.run_case('paths')


if __name__ == '__main__':
    unittest.main()
