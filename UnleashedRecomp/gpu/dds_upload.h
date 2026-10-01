#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>
#include <ddspp.h>

struct DdsUploadSlice
{
    uint32_t width, height, depth;
    size_t srcOffset, dstOffset;
    uint32_t srcRowPitch, dstRowPitch, rowCount, rowWidth;
};

struct DdsUploadLayout
{
    std::vector<DdsUploadSlice> slices;
    uint32_t arraySize = 0;
    size_t uploadSize = 0;
};

inline bool BuildRgba8UploadFootprint(uint32_t width, uint32_t height, uint32_t pitchAlignment,
    uint32_t& rowPitch, size_t& uploadSize)
{
    if (width == 0 || height == 0 || !std::has_single_bit(pitchAlignment))
        return false;
    const uint64_t pitch = (uint64_t(width) * 4 + pitchAlignment - 1) & ~uint64_t(pitchAlignment - 1);
    if (pitch > UINT32_MAX || pitch > std::numeric_limits<size_t>::max() / height)
        return false;
    rowPitch = uint32_t(pitch);
    uploadSize = size_t(pitch) * height;
    return true;
}

// Plan the existing tightly packed DDS -> aligned upload-buffer copy before
// allocating GPU resources. All mip/array payload and footprint arithmetic
// must fit; malformed headers must never cause an out-of-bounds memcpy.
inline bool BuildDdsUploadLayout(const ddspp::Descriptor& descriptor, size_t dataSize,
    uint32_t pitchAlignment, uint32_t placementAlignment, DdsUploadLayout& layout)
{
    layout = {};
    const auto& d = descriptor;
    if (d.headerSize > dataSize || d.width == 0 || d.height == 0 || d.depth == 0 ||
        d.arraySize == 0 || d.numMips == 0 || d.blockWidth == 0 || d.blockHeight == 0 ||
        d.bitsPerPixelOrBlock == 0 || !std::has_single_bit(pitchAlignment) || !std::has_single_bit(placementAlignment))
        return false;
    if (d.numMips > uint32_t(std::bit_width(std::max({d.width, d.height, d.depth}))))
        return false;

    const uint64_t arraySize = uint64_t(d.arraySize) * (d.type == ddspp::Cubemap ? 6 : 1);
    const size_t payloadSize = dataSize - d.headerSize;
    if (arraySize > UINT32_MAX || arraySize * d.numMips > UINT32_MAX || arraySize * d.numMips > payloadSize)
        return false;
    layout.arraySize = uint32_t(arraySize);
    size_t srcOffset = 0;
    size_t dstOffset = 0;
    constexpr size_t maxSize = std::numeric_limits<size_t>::max();

    for (uint32_t array = 0; array < layout.arraySize; ++array)
    {
        for (uint32_t mip = 0; mip < d.numMips; ++mip)
        {
            DdsUploadSlice slice{};
            slice.width = std::max(1u, d.width >> mip);
            slice.height = std::max(1u, d.height >> mip);
            slice.depth = std::max(1u, d.depth >> mip);
            slice.srcOffset = srcOffset;
            slice.dstOffset = dstOffset;
            const uint64_t blocks = slice.width / d.blockWidth + (slice.width % d.blockWidth != 0);
            const uint64_t rowBits = blocks * d.bitsPerPixelOrBlock;
            const uint64_t srcPitch = rowBits / 8 + (rowBits % 8 != 0);
            const uint64_t dstPitch = (srcPitch + pitchAlignment - 1) & ~uint64_t(pitchAlignment - 1);
            if (dstPitch > UINT32_MAX)
                return false;
            slice.srcRowPitch = uint32_t(srcPitch);
            slice.dstRowPitch = uint32_t(dstPitch);
            slice.rowCount = slice.height / d.blockHeight + (slice.height % d.blockHeight != 0);
            const uint64_t rowBlocks = dstPitch * 8 / d.bitsPerPixelOrBlock;
            if (rowBlocks > UINT32_MAX / d.blockWidth)
                return false;
            slice.rowWidth = uint32_t(rowBlocks) * d.blockWidth;

            // Divide before multiplying to bound both source reads and the
            // padded destination. srcPitch/rowCount/depth are all nonzero.
            if (srcPitch > (payloadSize - srcOffset) / slice.rowCount / slice.depth ||
                dstPitch > maxSize / slice.rowCount / slice.depth)
                return false;
            srcOffset += size_t(srcPitch) * slice.rowCount * slice.depth;
            const size_t destinationSize = size_t(dstPitch) * slice.rowCount * slice.depth;
            if (destinationSize > maxSize - (placementAlignment - 1))
                return false;
            const size_t placedSize = (destinationSize + placementAlignment - 1) & ~size_t(placementAlignment - 1);
            if (placedSize > maxSize - dstOffset)
                return false;
            dstOffset += placedSize;
            layout.slices.emplace_back(slice);
        }
    }
    layout.uploadSize = dstOffset;
    return true;
}
