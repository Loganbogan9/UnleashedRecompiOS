"""Exercise shader build plumbing with a fake compiler, never Metal execution."""
import pathlib
import subprocess
import sys
import tempfile


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


root, cmake, compiler = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix="metal build test ") as temporary:
    project = pathlib.Path(temporary)
    source = project / "source"
    source.mkdir()
    (source / "demo.metal").write_text("original shader")
    include = source / "shared.metali"
    include.write_text("original include")
    # This is the stale source-tree embed that formerly overrode iOS output.
    (source / "demo.metal.metallib.c").write_text("unsigned char g_demo_air[1] = {0};\n")
    fake = project / "fake-xcrun"
    fake.write_text("""#!/usr/bin/env python3
import pathlib, sys
args = sys.argv[1:]
output = pathlib.Path(args[args.index('-o') + 1])
if 'metal' in args:
    source = pathlib.Path(args[args.index('-c') + 1])
    output.write_bytes(bytes([0, 127, 128, 255]) + args[1].encode() + source.read_bytes() + (source.parent / 'shared.metali').read_bytes())
else:
    output.write_bytes(pathlib.Path(args[-1]).read_bytes())
""")
    fake.chmod(0o755)
    (source / "main.c").write_text("""#include <stdio.h>
#include <gpu/shader/msl/demo.metal.metallib.h>
int main(void) { return fwrite(g_demo_air, 1, sizeof(g_demo_air), stdout) == sizeof(g_demo_air) ? 0 : 1; }
""")
    (source / "CMakeLists.txt").write_text(f"""cmake_minimum_required(VERSION 3.20)
project(ShaderBuildTest C)
include("{root}/cmake/CompileMetalShader.cmake")
set(XCRUN_TOOL "{fake.as_posix()}")
add_executable(probe main.c)
unleashed_compile_metal_shader(probe "${{CMAKE_CURRENT_SOURCE_DIR}}/demo.metal" demo)
""")
    for sdk in ("macosx", "iphoneos"):
        build = project / sdk
        run(cmake, "-S", str(source), "-B", str(build), "-G", "Ninja",
            f"-DCMAKE_C_COMPILER={compiler}", f"-DUNLEASHED_RECOMP_APPLE_SDK={sdk}")
        run(cmake, "--build", str(build))
        expected = bytes([0, 127, 128, 255]) + sdk.encode() + b"original shaderoriginal include"
        assert subprocess.check_output([build / "probe"]) == expected
        # Included source changes must rebuild and re-embed the library.
        include.write_text("changed include")
        run(cmake, "--build", str(build))
        expected = bytes([0, 127, 128, 255]) + sdk.encode() + b"original shaderchanged include"
        assert subprocess.check_output([build / "probe"]) == expected
        include.write_text("original include")
        # Source changes must also rebuild, including without reconfiguration.
        (source / "demo.metal").write_text("changed shader")
        run(cmake, "--build", str(build))
        expected = bytes([0, 127, 128, 255]) + sdk.encode() + b"changed shaderoriginal include"
        assert subprocess.check_output([build / "probe"]) == expected
        (source / "demo.metal").write_text("original shader")

    empty = project / "empty.bin"
    empty.touch()
    failed = subprocess.run([cmake, f"-DINPUT={empty}", f"-DOUTPUT_C={project}/empty.c",
        f"-DOUTPUT_H={project}/empty.h", "-DSYMBOL=g_empty", "-P", f"{root}/cmake/EmbedBinary.cmake"],
        capture_output=True, text=True)
    assert failed.returncode != 0 and "empty binary" in failed.stderr
print("Shader SDK isolation, fresh embedding, include/source rebuilds and C linkage passed.")
