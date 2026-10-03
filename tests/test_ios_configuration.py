"""Render the actual iOS plist and exercise selected-config preprocessor flags.

Uses host CMake/Clang; does not generate an Xcode project or invoke Apple SDKs.
"""
import json
import pathlib
import plistlib
import re
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
cmake, compiler = sys.argv[2:]
presets = json.loads((root / 'CMakePresets.json').read_text())
configure = {p['name']: p for p in presets['configurePresets']}
build = {p['name']: p for p in presets['buildPresets']}
for name, configuration in (('ios-xcode-debug', 'Debug'), ('ios-xcode-relwithdebinfo', 'RelWithDebInfo'), ('ios-xcode-release', 'Release')):
    assert build[name]['configuration'] == configuration
assert configure['ios-base']['cacheVariables']['CMAKE_OSX_DEPLOYMENT_TARGET'] == '16.0'
for language in ('C', 'CXX'):
    assert '-DNDEBUG' in configure['ios-xcode-release']['cacheVariables'][f'CMAKE_{language}_FLAGS_RELEASE'].split()

with tempfile.TemporaryDirectory(prefix='ios configuration ') as folder:
    work = pathlib.Path(folder)
    script = work / 'plist.cmake'
    script.write_text(f'''
include("{root.as_posix()}/UnleashedRecomp/version.cmake")
CreateVersionString(VERSION_TXT "{root.as_posix()}/UnleashedRecomp/res/version.txt" OUTPUT_CSV 1 OUTPUT_VAR bundle_version)
string(REPLACE "," "." bundle_version "${{bundle_version}}")
set(MACOSX_BUNDLE_EXECUTABLE_NAME UnleashedRecomp)
set(MACOSX_BUNDLE_GUI_IDENTIFIER test.unleashed)
set(MACOSX_BUNDLE_BUNDLE_NAME UnleashedRecomp)
set(MACOSX_BUNDLE_SHORT_VERSION_STRING "${{bundle_version}}")
set(MACOSX_BUNDLE_BUNDLE_VERSION "${{bundle_version}}")
set(CMAKE_OSX_DEPLOYMENT_TARGET 16.0)
configure_file("{root.as_posix()}/UnleashedRecomp/res/ios/Info.plist.in" "{work.as_posix()}/Info.plist")
''')
    subprocess.run([cmake, '-P', str(script)], check=True, capture_output=True)
    info = plistlib.loads((work / 'Info.plist').read_bytes())
    assert info['MinimumOSVersion'] == '16.0'
    assert info['CFBundleSupportedPlatforms'] == ['iPhoneOS']
    assert info['LSRequiresIPhoneOS'] and info['UIFileSharingEnabled']
    assert info['CADisableMinimumFrameDurationOnPhone'] is True
    for key in ('CFBundleVersion', 'CFBundleShortVersionString'):
        assert re.fullmatch(r'[0-9]+(?:\.[0-9]+){0,2}', info[key])
    assert '$' not in (work / 'Info.plist').read_text()

    definition = re.search(r'^add_compile_definitions\("\$<\$<CONFIG:Debug>:_DEBUG>"\)$',
                           (root / 'CMakeLists.txt').read_text(), re.MULTILINE)
    assert definition, 'Debug definition must follow the selected configuration'
    (work / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(ConfigTest CXX)
{definition[0]}
add_executable(config config.cpp)
target_compile_definitions(config PRIVATE EXPECT_DEBUG=$<IF:$<CONFIG:Debug>,1,0>)
''')
    (work / 'config.cpp').write_text('''
#ifdef _DEBUG
#define ACTUAL_DEBUG 1
#else
#define ACTUAL_DEBUG 0
#endif
static_assert(EXPECT_DEBUG == ACTUAL_DEBUG);
int main() { return 0; }
''')
    output = work / 'build'
    subprocess.run([cmake, '-G', 'Ninja Multi-Config', '-S', str(work), '-B', str(output),
                    f'-DCMAKE_CXX_COMPILER={compiler}'], check=True, capture_output=True)
    for configuration in ('Debug', 'Release'):
        subprocess.run([cmake, '--build', str(output), '--config', configuration], check=True, capture_output=True)
        subprocess.run([str(output / configuration / 'config')], check=True)
print('iOS plist versions/platforms and selected build configuration checks passed')
