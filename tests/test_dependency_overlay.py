"""Check reproducible patch overlays without mutating dependency checkouts."""
import hashlib
import pathlib
import shutil
import subprocess
import sys
import tempfile

root, cmake, binary = map(pathlib.Path, sys.argv[1:])
with tempfile.TemporaryDirectory(prefix="dependency overlay ", dir=binary) as temporary:
    temporary = pathlib.Path(temporary)
    source = temporary / "upstream"
    source.mkdir()
    for name in ("plume_metal.cpp", "plume_metal.h"):
        shutil.copyfile(root / "thirdparty/plume" / name, source / name)
    (source / "contrib").mkdir()
    (source / "contrib/kept.txt").write_text("linked dependency")
    (source / ".git").write_text("gitdir: deliberately-not-a-repository")
    project = temporary / "project"
    project.mkdir()
    (project / "CMakeLists.txt").write_text(f"""cmake_minimum_required(VERSION 3.20)
project(OverlayTest NONE)
include("{root.as_posix()}/cmake/PrepareDependency.cmake")
file(GLOB patches "{root.as_posix()}/patches/dependencies/plume-*.patch")
list(SORT patches)
unleashed_prepare_dependency(patched plume "{source.as_posix()}" PATCHES ${{patches}})
file(WRITE "${{CMAKE_BINARY_DIR}}/overlay.txt" "${{patched}}")
""")
    build = temporary / "build"
    def configure():
        subprocess.run([str(cmake), "-S", str(project), "-B", str(build)], check=True, capture_output=True, text=True)
        return pathlib.Path((build / "overlay.txt").read_text())
    hashes = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in source.iterdir() if p.is_file()}
    overlay = configure()
    text = (overlay / "plume_metal.cpp").read_text()
    assert "pushConstants[rangeIndex].data.resize(alignUp(range.size));" in text
    assert "dispatch_release(dispatchData);" in text
    assert "functionName->retain();" in text
    assert "activeRenderState = nullptr;" in text
    assert (overlay / "contrib/kept.txt").read_text() == "linked dependency"
    timestamp = (overlay / "plume_metal.cpp").stat().st_mtime_ns
    assert configure() == overlay
    assert (overlay / "plume_metal.cpp").stat().st_mtime_ns == timestamp
    assert hashes == {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in hashes}
    # Unrelated local edits survive in a fresh patched overlay.
    cpp = source / "plume_metal.cpp"
    cpp.write_text("// local user change\n" + cpp.read_text())
    changed = configure()
    assert changed != overlay and (changed / "plume_metal.cpp").read_text().startswith("// local user change")
    # Conflicting edits produce an actionable configure error, never overwrite.
    cpp.write_text(cpp.read_text().replace("data.resize(range.size)", "data.resize(range.size + 1)"))
    saved = cpp.read_bytes()
    failed = subprocess.run([str(cmake), "-S", str(project), "-B", str(build)], capture_output=True, text=True)
    assert failed.returncode != 0 and "Original checkout is untouched" in failed.stderr
    assert cpp.read_bytes() == saved
print("Patch overlays, idempotent configuration and preservation of local edits passed.")
