"""Test pinned Metal backports and their portable push-constant storage code.

This does not compile Metal, Objective-C, or MSL. The storage setters are taken
verbatim from the patched source and compiled against a minimal portable shell.
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile


def run(*arguments):
    return subprocess.run(arguments, check=True, capture_output=True, text=True)


def function(source, signature):
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


root = pathlib.Path(sys.argv[1])
compiler = sys.argv[2]
flags = sys.argv[3:]
with tempfile.TemporaryDirectory(prefix="plume metal test ") as directory:
    work = pathlib.Path(directory)
    for name in ("plume_metal.cpp", "plume_metal.h"):
        shutil.copy2(root / "thirdparty/plume" / name, work / name)
    patches = sorted((root / "patches/dependencies").glob("plume-*.patch"))
    assert patches, "No Metal backports found"
    for patch in patches:
        run("git", "-C", str(work), "apply", "--check", str(patch))
        run("git", "-C", str(work), "apply", str(patch))
    source = (work / "plume_metal.cpp").read_text()
    header = (work / "plume_metal.h").read_text()
    assert "ExtendedRenderTexture *interfaceTexture = static_cast<ExtendedRenderTexture *>(textureBarrier.texture)" in source
    assert "NS::AutoreleasePool" not in source
    assert "functionName->retain();" in source
    assert "debugName->retain();" in source
    assert "dispatch_release(dispatchData);" in source
    assert "if (waitSemaphoreCount > 0)" in source
    assert "nullBuffer.reset();" in source
    assert "activeRenderState = nullptr;" in source
    assert "setTexture(nullptr, argumentIndex);" in source
    assert "std::lock_guard lock(descriptorMutex);" in source
    assert "std::lock_guard lock(descriptorSet->descriptorMutex);" in source
    assert "std::atomic<bool> needsCommit" in header
    assert "mtl->argumentBuffersSupport() == MTL::ArgumentBuffersTier2" in source
    assert "assert(device->capabilities.bufferDeviceAddress" in source
    # Retain the existing Xenos synchronization, residency and render-pass
    # policies; these tests also detect accidentally applying a newer API.
    assert "MetalCommandQueue *commandQueue, const RenderWindow renderWindow" in source
    assert "GPUFamilyApple6" in source
    assert "endOtherEncoders(EncoderType::None);" in source
    probe = r'''
#include <plume_render_interface_types.h>
#include <cstring>
#include <iostream>
namespace plume {
static constexpr uint32_t MAX_PUSH_CONSTANT_BINDINGS = 4;
struct Layout { std::vector<RenderPushConstantRange> pushConstantRanges; };
struct MetalCommandList {
    struct PushConstantData : RenderPushConstantRange { std::vector<uint8_t> data; };
    Layout* activeComputePipelineLayout = nullptr;
    Layout* activeGraphicsPipelineLayout = nullptr;
    std::vector<PushConstantData> pushConstants;
    struct Flags { uint32_t pushConstants = 0; } dirtyComputeState, dirtyGraphicsState;
    void setComputePushConstants(uint32_t, const void*, uint32_t, uint32_t);
    void setGraphicsPushConstants(uint32_t, const void*, uint32_t, uint32_t);
};
'''
    probe += function(source, "constexpr uint64_t alignUp(") + "\n"
    probe += function(source, "void MetalCommandList::setComputePushConstants(") + "\n"
    probe += function(source, "void MetalCommandList::setGraphicsPushConstants(") + "\n}\n"
    probe += r'''
int main() {
    using namespace plume;
    for (uint32_t size : {1u, 4u, 8u, 12u, 24u, 32u, 36u}) {
        Layout layout;
        RenderPushConstantRange range{};
        range.size = size;
        range.binding = 0;
        range.stageFlags = RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL;
        layout.pushConstantRanges.push_back(range);
        MetalCommandList command;
        command.activeGraphicsPipelineLayout = command.activeComputePipelineLayout = &layout;
        std::vector<uint8_t> input(size, 0xa5);
        for (bool compute : {false, true}) {
            if (compute) command.setComputePushConstants(0, input.data(), 0, 0);
            else command.setGraphicsPushConstants(0, input.data(), 0, 0);
            const auto& constant = command.pushConstants[0];
            assert(constant.size % 16 == 0);
            assert(constant.data.size() >= constant.size);
            // Model the actual setBytes/setVertexBytes read. ASan detects
            // the original setter's overread for non-16-byte ranges.
            std::vector<uint8_t> uploaded(constant.size);
            std::memcpy(uploaded.data(), constant.data.data(), constant.size);
            for (uint32_t i = 0; i < size; ++i) assert(uploaded[i] == 0xa5);
            for (uint32_t i = size; i < constant.size; ++i) assert(uploaded[i] == 0);
            uint8_t update = 0x5a;
            if (compute) command.setComputePushConstants(0, &update, size - 1, 1);
            else command.setGraphicsPushConstants(0, &update, size - 1, 1);
            assert(command.pushConstants[0].data[size - 1] == 0x5a);
            if (size > 1) assert(command.pushConstants[0].data[0] == 0xa5);
        }
    }
    std::cout << "Metal push-constant storage and backport checks passed\n";
}
'''
    (work / "probe.cpp").write_text(probe)
    run(compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags,
        "-I" + str(root / "thirdparty/plume"), str(work / "probe.cpp"), "-o", str(work / "probe"))
    print(run(str(work / "probe")).stdout, end="")
    # Every patch must reverse cleanly and restore the exact pinned sources.
    for patch in reversed(patches):
        run("git", "-C", str(work), "apply", "--reverse", "--check", str(patch))
        run("git", "-C", str(work), "apply", "--reverse", str(patch))
    for name in ("plume_metal.cpp", "plume_metal.h"):
        assert (work / name).read_bytes() == (root / "thirdparty/plume" / name).read_bytes()
