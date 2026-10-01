"""Verify movie shader bindings against compiled host constant-buffer offsets."""
import pathlib
import re
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1])
compiler = sys.argv[2]
host_source = (root / "UnleashedRecomp/gpu/video.cpp").read_text()
start = host_source.index("struct SharedConstants\n")
end = host_source.index("\n};", start) + 3
declaration = host_source[start:end]
with tempfile.TemporaryDirectory(prefix="shader layout test ") as directory:
    work = pathlib.Path(directory)
    (work / "layout.cpp").write_text("""
#include <cstdint>
#include <cstddef>
#include <iostream>
""" + declaration + """
int main() {
    std::cout << offsetof(SharedConstants, texture2DIndices) << ' '
              << offsetof(SharedConstants, samplerIndices) << '\\n';
}
""")
    subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    str(work / "layout.cpp"), "-o", str(work / "layout")], check=True)
    texture_base, sampler_base = map(int, subprocess.check_output([work / "layout"]).split())

msl = (root / "UnleashedRecomp/gpu/shader/msl/movie_common.metali").read_text()
hlsl = (root / "UnleashedRecomp/gpu/shader/hlsl/movie_common.hlsli").read_text()
for index in range(5):
    for kind, base in (("Resource", texture_base), ("Sampler", sampler_base)):
        name = f"Tex{index}_{kind}DescriptorIndex"
        expected = base + index * 4
        for source in (msl, hlsl):
            actual = int(re.search(r"#define " + name + r"[^\n]*SharedConstants \+ (\d+)", source)[1])
            assert actual == expected, f"{name} loads byte {actual}, host field is at {expected}"
        register, component = re.search(r"uint " + name + r" : packoffset\(c(\d+)\.([xyzw])\)", hlsl).groups()
        actual = int(register) * 16 + "xyzw".index(component) * 4
        assert actual == expected, f"{name} cbuffer byte {actual}, host field is at {expected}"
print("Movie shader and host constant-buffer layouts match")
