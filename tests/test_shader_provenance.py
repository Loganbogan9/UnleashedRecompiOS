import pathlib
import subprocess
import sys
import tempfile

root, cmake = sys.argv[1:]
root = pathlib.Path(root).resolve().as_posix()
with tempfile.TemporaryDirectory(prefix="shader provenance ") as directory:
    directory = pathlib.Path(directory)
    cache = directory / "cache.cpp"
    script = directory / "check.cmake"
    script.write_text(f'include("{root}/cmake/ValidateIOSShaderCache.cmake")\nunleashed_validate_ios_shader_cache("{cache.as_posix()}" "16.0")\n')
    for sdk, triple, valid in (("iphoneos", "air64-apple-ios16.0", True),
            ("macosx", "air64-apple-macos13.0", False),
            ("iphoneos", "air64-apple-ios18.0", False),
            ("iphoneos", "air64-apple-macos16.0", False)):
        cache.write_text(f'const char g_airCacheTargetSdk[] = "{sdk}";\nconst char g_airCacheTargetTriple[] = "{triple}";\n')
        result = subprocess.run([cmake, "-P", str(script)], capture_output=True, text=True)
        assert (result.returncode == 0) == valid, result.stderr
    cache.write_text('#include "shader_cache.h"\nShaderCacheEntry g_shaderCacheEntries[] = {};\n')
    assert subprocess.run([cmake, "-P", str(script)], capture_output=True).returncode != 0
    cache.unlink()
    assert subprocess.run([cmake, "-P", str(script)], capture_output=True).returncode != 0
    cache.write_text('const char g_airCacheTargetSdk[] = "iphoneos";\nconst char g_airCacheTargetTriple[] = "air64-apple-ios16.0";\n')
    (directory / "CMakeLists.txt").write_text(f'cmake_minimum_required(VERSION 3.20)\nproject(provenance NONE)\ninclude("{root}/cmake/ValidateIOSShaderCache.cmake")\nunleashed_validate_ios_shader_cache("{cache.as_posix()}" "16.0")\n')
    build = directory / "build"
    subprocess.run([cmake, "-S", str(directory), "-B", str(build)], check=True, capture_output=True)
    cache.write_text('const char g_airCacheTargetSdk[] = "macosx";\nconst char g_airCacheTargetTriple[] = "air64-apple-macos13.0";\n')
    result = subprocess.run([cmake, "--build", str(build)], capture_output=True, text=True)
    assert result.returncode != 0 and "iPhoneOS game shader cache" in result.stderr, result.stderr
print("iOS game shader SDK/deployment-target provenance checks passed.")
