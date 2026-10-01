"""Compile actual Metal descriptor bookkeeping/traversal against portable mocks.

The optional --benchmark measures traversal only, without any Metal API costs.
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile


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
flags = [flag for flag in sys.argv[3:] if flag != "--benchmark"]
benchmark = "--benchmark" in sys.argv[3:]
with tempfile.TemporaryDirectory(prefix="Metal descriptor test ") as directory:
    work = pathlib.Path(directory)
    for name in ("plume_metal.cpp", "plume_metal.h"):
        shutil.copy2(root / "thirdparty/plume" / name, work / name)
    for patch in sorted((root / "patches/dependencies").glob("plume-*.patch")):
        subprocess.run(["git", "-C", str(work), "apply", str(patch)], check=True)
    source = (work / "plume_metal.cpp").read_text()
    header = (work / "plume_metal.h").read_text()
    probe = r'''
#include <plume_render_interface_types.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <tuple>
namespace MTL {
using ResourceUsage = unsigned;
static constexpr ResourceUsage ResourceUsageRead = 1, ResourceUsageWrite = 2, DataTypeNone = 0;
static constexpr unsigned RenderStageVertex = 1, RenderStageFragment = 2;
struct Resource { uint64_t id; };
using Declaration = std::tuple<uint64_t, unsigned, unsigned>;
struct CommandEncoder { std::vector<Declaration> declarations; };
struct ComputeCommandEncoder : CommandEncoder {
    void useResource(const Resource* resource, unsigned usage) {
        declarations.emplace_back(resource->id, usage, 0);
    }
};
struct RenderCommandEncoder : CommandEncoder {
    void useResource(const Resource* resource, unsigned usage, unsigned stages) {
        declarations.emplace_back(resource->id, usage, stages);
    }
    void useResource(const Resource* resource, unsigned usage) { useResource(resource, usage, 3); }
};
}
namespace plume {
struct MetalDescriptorSet {
'''
    probe += function(header, "struct ResourceEntry") + ";\n"
    probe += r'''
    std::vector<ResourceEntry> resourceEntries;
    std::vector<uint64_t> activeResourceWords;
    uint32_t activeResourceCount = 0;
    mutable std::mutex descriptorMutex;
    void* residencySet = nullptr;
    void setResourceEntry(uint32_t, MTL::Resource*, RenderDescriptorRangeType);
};
struct MetalDevice {
    void* gpuAddressableResidencySet = nullptr;
    std::mutex gpuAddressableResourcesMutex;
    std::vector<MTL::Resource*> gpuAddressableResources;
};
struct MetalCommandList {
    MetalDevice* device;
    std::vector<const MetalDescriptorSet*> currentEncoderDescriptorSets;
    void bindEncoderResources(MTL::CommandEncoder*, bool);
};
'''
    probe += function(source, "MTL::ResourceUsage mapResourceUsage(") + "\n"
    probe += function(source, "void MetalDescriptorSet::setResourceEntry(") + "\n"
    probe += function(source, "void MetalCommandList::bindEncoderResources(") + "\n}\n"
    probe += r'''
using namespace plume;

// The old full-capacity loop, with the same observable resource/type checksum
// as the new loop. No inline keeps repeated benchmark iterations observable.
[[gnu::noinline]] uint64_t fullTraversal(const MetalDescriptorSet& set) {
    uint64_t result = 0;
    for (const auto& entry : set.resourceEntries)
        if (entry.resource != nullptr) result += entry.resource->id + mapResourceUsage(entry.type);
    return result;
}
[[gnu::noinline]] uint64_t liveTraversal(const MetalDescriptorSet& set) {
    if (set.activeResourceCount >= (set.resourceEntries.size() + 3) / 4) return fullTraversal(set);
    uint64_t result = 0;
    for (size_t wordIndex = 0; wordIndex < set.activeResourceWords.size(); ++wordIndex) {
        uint64_t word = set.activeResourceWords[wordIndex];
        while (word != 0) {
            const auto& entry = set.resourceEntries[wordIndex * 64 + __builtin_ctzll(word)];
            result += entry.resource->id + mapResourceUsage(entry.type);
            word &= word - 1;
        }
    }
    return result;
}
[[gnu::noinline]] void rawUpdate(MetalDescriptorSet& set, uint32_t index, MTL::Resource* resource, RenderDescriptorRangeType type) {
    set.resourceEntries[index].resource = resource;
    set.resourceEntries[index].type = type;
}
[[gnu::noinline]] void bitmapUpdate(MetalDescriptorSet& set, uint32_t index, MTL::Resource* resource, RenderDescriptorRangeType type) {
    set.setResourceEntry(index, resource, type);
}

void verify(MetalDescriptorSet& set, MetalCommandList& command) {
    uint32_t activeCount = 0;
    for (size_t index = 0; index < set.resourceEntries.size(); ++index) {
        const bool active = (set.activeResourceWords[index / 64] & (uint64_t(1) << (index % 64))) != 0;
        assert(active == (set.resourceEntries[index].resource != nullptr));
        activeCount += active;
    }
    assert(set.activeResourceCount == activeCount);
    for (bool compute : {true, false}) {
        std::vector<MTL::Declaration> expected;
        if (command.device->gpuAddressableResidencySet == nullptr) {
            for (auto* resource : command.device->gpuAddressableResources)
                expected.emplace_back(resource->id, MTL::ResourceUsageRead, compute ? 0 : 3);
        }
        if (set.residencySet == nullptr) {
            // Exact declarations from the previous full-capacity algorithm.
            for (const auto& entry : set.resourceEntries)
                if (entry.resource != nullptr)
                    expected.emplace_back(entry.resource->id, mapResourceUsage(entry.type), compute ? 0 : 3);
        }
        MTL::ComputeCommandEncoder computeEncoder;
        MTL::RenderCommandEncoder renderEncoder;
        MTL::CommandEncoder* encoder = compute ? static_cast<MTL::CommandEncoder*>(&computeEncoder)
                                              : static_cast<MTL::CommandEncoder*>(&renderEncoder);
        command.bindEncoderResources(encoder, compute);
        // Compare exact order too, including duplicate native resources with
        // different usage flags. No assumptions about declaration ordering.
        assert(encoder->declarations == expected);
    }
    assert(fullTraversal(set) == liveTraversal(set));
}

int main(int argc, char**) {
    constexpr uint32_t capacity = 65536;
    MetalDescriptorSet set;
    set.resourceEntries.resize(capacity);
    set.activeResourceWords.resize((capacity + 63) / 64);
    std::vector<std::unique_ptr<MTL::Resource>> resources(capacity);
    MetalDevice device;
    MTL::Resource global{999999};
    device.gpuAddressableResources.push_back(&global);
    MetalCommandList command{&device, {&set}};
    for (uint32_t smallCapacity : {1u, 7u, 63u, 64u, 65u, 129u}) {
        MetalDescriptorSet small;
        small.resourceEntries.resize(smallCapacity);
        small.activeResourceWords.resize((smallCapacity + 63) / 64);
        command.currentEncoderDescriptorSets = {&small};
        for (uint32_t index = 0; index < smallCapacity; ++index)
            small.setResourceEntry(index, &global, RenderDescriptorRangeType::TEXTURE);
        verify(small, command);
        for (uint32_t index = 0; index < smallCapacity; ++index)
            small.setResourceEntry(index, nullptr, RenderDescriptorRangeType::TEXTURE);
        small.setResourceEntry(smallCapacity - 1, &global, RenderDescriptorRangeType::READ_WRITE_TEXTURE);
        verify(small, command);
    }
    command.currentEncoderDescriptorSets = {&set};
    auto update = [&](uint32_t index, bool clear, uint64_t id) {
        // Mirror setDescriptor's lock and lifetime. Clearing permits resource
        // destruction; a stale live bit will cause an ASan use-after-free.
        std::lock_guard lock(set.descriptorMutex);
        const auto type = id % 2 == 0 ? RenderDescriptorRangeType::TEXTURE
                                     : RenderDescriptorRangeType::READ_WRITE_TEXTURE;
        if (clear) {
            set.setResourceEntry(index, nullptr, type);
            resources[index].reset();
        } else {
            auto replacement = std::make_unique<MTL::Resource>(MTL::Resource{id});
            set.setResourceEntry(index, replacement.get(), type);
            resources[index] = std::move(replacement);
        }
    };
    verify(set, command);
    for (uint32_t index : {0u, 65535u, 7u, 42u}) update(index, false, index);
    update(7, true, 0); // Middle removal.
    update(42, true, 0); // Tail removal.
    update(0, true, 0); // First removal.
    update(0, true, 0); // Already-empty removal.
    update(65535, false, 123); // Occupied-slot replacement.
    set.setResourceEntry(1, &global, RenderDescriptorRangeType::READ_WRITE_TEXTURE);
    set.setResourceEntry(2, &global, RenderDescriptorRangeType::TEXTURE);
    verify(set, command); // Mixed-usage aliases retain the original order.
    set.setResourceEntry(1, nullptr, RenderDescriptorRangeType::TEXTURE);
    set.setResourceEntry(2, nullptr, RenderDescriptorRangeType::TEXTURE);
    verify(set, command);
    std::mt19937 random(0x360);
    for (uint32_t iteration = 0; iteration < 12000; ++iteration) {
        const uint32_t index = random() % capacity;
        update(index, random() % 3 == 0, uint64_t(iteration) * capacity + index);
        if (iteration % 64 == 0) verify(set, command);
    }
    verify(set, command);
    // Exercise dense fallback and both sides of its threshold, including
    // word boundaries and resource replacement/removal at the endpoints.
    for (uint32_t index = 0; index < capacity / 4 + 1; ++index) update(index, false, index);
    verify(set, command);
    for (uint32_t index = 0; index < capacity; ++index) update(index, true, 0);
    for (uint32_t index = 0; index < capacity / 4 - 1; ++index) update(index, false, index);
    verify(set, command);
    update(capacity / 4 - 1, false, 42);
    verify(set, command);
    // Newer OS paths use residency sets and must not issue fallback calls.
    set.residencySet = &global;
    device.gpuAddressableResidencySet = &global;
    verify(set, command);
    set.residencySet = device.gpuAddressableResidencySet = nullptr;
    for (uint32_t index = 0; index < capacity; ++index) update(index, true, 0);
    assert(std::all_of(set.activeResourceWords.begin(), set.activeResourceWords.end(), [](uint64_t word) { return word == 0; }));
    verify(set, command); // ASan catches any dereference of retired resources.
    std::cout << "Metal descriptor declarations and lifetime bookkeeping match full scans\n";

    if (argc > 1) {
        for (uint32_t active : {256u, 1024u, 8192u, 16384u, 32768u, 65536u}) {
            for (uint32_t index = 0; index < capacity; ++index) update(index, true, 0);
            for (uint32_t index = 0; index < active; ++index)
                update((index * 7919) % capacity, false, index + 1);
            constexpr uint32_t iterations = 4000;
            auto measure = [&](auto traversal) {
                const auto start = std::chrono::steady_clock::now();
                uint64_t checksum = 0;
                for (uint32_t iteration = 0; iteration < iterations; ++iteration) checksum += traversal(set);
                const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
                return std::pair{elapsed / iterations, checksum};
            };
            auto [fullTime, fullSum] = measure(fullTraversal);
            auto [liveTime, liveSum] = measure(liveTraversal);
            assert(fullSum == liveSum);
            std::cout << "capacity=" << capacity << " live=" << active
                      << " full_us=" << fullTime << " live_us=" << liveTime
                      << " ratio=" << fullTime / liveTime << " checksum=" << liveSum << '\n';
        }
        std::vector<uint32_t> churnIndices(4096);
        for (auto& index : churnIndices) index = random() % capacity;
        auto measureUpdates = [&](auto updater) {
            const auto start = std::chrono::steady_clock::now();
            constexpr uint32_t repeats = 1000;
            for (uint32_t repeat = 0; repeat < repeats; ++repeat) {
                for (uint32_t index : churnIndices)
                    updater(set, index, repeat % 2 ? &global : nullptr, RenderDescriptorRangeType::TEXTURE);
            }
            const auto elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
            return std::pair{elapsed / repeats / churnIndices.size(), fullTraversal(set)};
        };
        auto [rawTime, rawSum] = measureUpdates(rawUpdate);
        auto [bitmapTime, bitmapSum] = measureUpdates(bitmapUpdate);
        assert(rawSum == bitmapSum);
        std::cout << "update_churn raw_ns=" << rawTime << " bitmap_ns=" << bitmapTime
                  << " ratio=" << bitmapTime / rawTime << " checksum=" << bitmapSum << '\n';
    }
}
'''
    (work / "probe.cpp").write_text(probe)
    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags,
                    "-I" + str(root / "thirdparty/plume"), str(work / "probe.cpp"), "-o", str(work / "probe")], check=True)
    subprocess.run([str(work / "probe"), *(["--benchmark"] if benchmark else [])], check=True)
