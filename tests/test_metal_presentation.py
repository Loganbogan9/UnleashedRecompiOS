"""Run the patched Metal swapchain against controlled drawable/GPU callbacks.

The production constructor, acquire, present, wait and diagnostic functions are
compiled with Release flags. Only Metal/Objective-C objects are replaced by
test doubles; callback timing and a finite drawable pool are controlled here.
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile


def block(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[start:end]


def run(*args, timeout=60):
    result = subprocess.run(args, capture_output=True, text=True, timeout=timeout,
                            env={**os.environ, "GIT_CEILING_DIRECTORIES": tempfile.gettempdir()})
    if result.returncode:
        sys.stderr.write(result.stdout + result.stderr)
        raise RuntimeError(f"Command failed ({result.returncode}): {args}")
    return result.stdout


root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2]
with tempfile.TemporaryDirectory(prefix="metal presentation ") as directory:
    work = pathlib.Path(directory)
    for name in ("plume_metal.cpp", "plume_metal.h"):
        shutil.copyfile(root / "thirdparty/plume" / name, work / name)
    for patch in sorted((root / "patches/dependencies").glob("plume-*.patch")):
        run("git", "-C", str(work), "apply", "--check", str(patch))
        run("git", "-C", str(work), "apply", str(patch))
    source = (work / "plume_metal.cpp").read_text()
    header = (work / "plume_metal.h").read_text()
    implementation = "namespace plume {\n"
    # Preserve the production state fields and declarations. Remove only the
    # abstract graphics-interface inheritance, which the test doubles omit.
    implementation += block(header, "struct MetalSwapChain : RenderSwapChain").replace(
        " : RenderSwapChain", "").replace(" override", "") + ";\n"
    implementation += "static std::atomic<MetalDiagnosticCallback> metalDiagnosticCallback{nullptr};\n"
    implementation += block(source, "struct MetalSubmissionState") + ";\n"
    for signature in (
        "void setMetalDiagnosticCallback(",
        "static void reportMetalDiagnostic(",
        "static void reportMetalCommandBufferError(",
        "static MetalSubmissionState &metalSubmissionState(",
        "static void waitForMetalAppActive(",
        "static void commitMetalCommandBuffer(",
        "void setMetalAppActive(",
        "MetalDrawable::~MetalDrawable(",
        "MetalSwapChain::MetalSwapChain(",
        "MetalSwapChain::~MetalSwapChain(",
        "bool MetalSwapChain::acquireTexture(",
        "bool MetalSwapChain::present(",
        "void MetalSwapChain::wait(",
        "void MetalSwapChain::getWindowSize(",
        "void MetalCommandList::commit(",
    ):
        implementation += block(source, signature) + "\n"
    implementation += "}\n"
    (work / "metal_swapchain.h").write_text(implementation)
    run(compiler, "-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
        *sys.argv[3:], "-O2", "-DNDEBUG", "-UTARGET_OS_IPHONE", "-DTARGET_OS_IPHONE=1", "-I" + str(work),
        str(root / "tests/portable/metal_presentation.cpp"), "-o", str(work / "probe"))
    # All native submissions and queue reservations must go through the gate.
    # Descriptor/residency commits do not submit native command buffers.
    for name in ("presentBuffer", "acquireBuffer", "cmdBuffer", "mtl"):
        assert name + "->commit();" not in source
        assert name + "->enqueue();" not in source
    for case in ("ownership", "unavailable", "callbacks", "diagnostics", "lifecycle"):
        print(run(str(work / "probe"), case, timeout=15), end="")
