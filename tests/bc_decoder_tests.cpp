#include <gpu/bc_decoder.h>
#include <gpu/dds_header.h>
#include <gpu/dds_upload.h>

#include <array>
#include <cassert>
#include <iostream>
#include <random>

using BlockCompression::Format;

int main()
{
    std::vector<uint8_t> pixels;
    // Reversed color endpoints with all pixels selecting entry 3. BC1 must
    // remain transparent; BC2/3 must interpolate a third of blue, two thirds
    // of red. This is the case the previous iOS fallback decoded as black.
    const std::array<uint8_t, 8> colors{0x1f, 0x00, 0x00, 0xf8, 0xff, 0xff, 0xff, 0xff};
    assert(BlockCompression::DecodeMip(colors, 4, 4, Format::BC1, pixels));
    for (uint8_t pixel : pixels) assert(pixel == 0);

    std::array<uint8_t, 16> block{};
    std::copy(colors.begin(), colors.end(), block.begin() + 8);
    std::fill_n(block.begin(), 8, 0xff);
    assert(BlockCompression::DecodeMip(block, 4, 4, Format::BC2, pixels));
    for (size_t i = 0; i < 16; ++i)
    {
        assert(pixels[i * 4] == 170 && pixels[i * 4 + 1] == 0);
        assert(pixels[i * 4 + 2] == 85 && pixels[i * 4 + 3] == 255);
    }
    // DXT3 explicit 4-bit alpha, low nibble first.
    block[0] = 0xa1;
    assert(BlockCompression::DecodeMip(block, 4, 4, Format::BC2, pixels));
    assert(pixels[3] == 17 && pixels[7] == 170);

    std::fill_n(block.begin(), 8, 0);
    block[0] = 200;
    block[1] = 100;
    assert(BlockCompression::DecodeMip(block, 4, 4, Format::BC3, pixels));
    for (size_t i = 0; i < 16; ++i)
        assert(pixels[i * 4] == 170 && pixels[i * 4 + 2] == 85 && pixels[i * 4 + 3] == 200);

    // Both alpha interpolation modes, including special 0/255 endpoints.
    std::array<uint8_t, 8> channel{0, 255, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    assert(BlockCompression::DecodeMip(channel, 1, 1, Format::BC4, pixels));
    assert(pixels.size() == 4 && pixels[0] == 255 && pixels[3] == 255);
    channel[0] = 255;
    channel[1] = 0;
    assert(BlockCompression::DecodeMip(channel, 1, 1, Format::BC4, pixels));
    assert(pixels[0] == 36);
    std::copy(channel.begin(), channel.end(), block.begin());
    channel[0] = 0;
    channel[1] = 255;
    std::copy(channel.begin(), channel.end(), block.begin() + 8);
    assert(BlockCompression::DecodeMip(block, 3, 2, Format::BC5, pixels));
    assert(pixels.size() == 24 && pixels[0] == 36 && pixels[1] == 255 && pixels[2] == 0 && pixels[3] == 255);

    std::array<uint8_t, 16> twoColors{};
    std::copy(colors.begin(), colors.end(), twoColors.begin());
    twoColors[8] = 0;
    twoColors[9] = 0xf8;
    assert(BlockCompression::DecodeMip(twoColors, 5, 3, Format::BC1, pixels));
    assert(pixels.size() == 60 && pixels[0] == 0 && pixels[4 * 4] == 255);
    for (auto format : {Format::BC1, Format::BC2, Format::BC3, Format::BC4, Format::BC5})
    {
        const size_t blockSize = format == Format::BC1 || format == Format::BC4 ? 8 : 16;
        for (size_t size = 0; size < blockSize; ++size)
            assert(!BlockCompression::DecodeMip(std::span(block).first(size), 1, 1, format, pixels));
        assert(!BlockCompression::DecodeMip(block, 0, 1, format, pixels));
        assert(!BlockCompression::DecodeMip(block, UINT32_MAX, UINT32_MAX, format, pixels));
    }
    assert(!BlockCompression::DecodeMip(std::span(twoColors).first(15), 5, 3, Format::BC1, pixels));

    // A minimal legacy DDS is 128-byte header + 8-byte BC1 mip. ddspp used
    // to unconditionally read a 20-byte DX10 header past this allocation.
    ddspp::Header header{};
    ddspp::HeaderDXT10 extension{};
    ddspp::encode_header(ddspp::BC1_UNORM, 1, 1, 1, ddspp::Texture2D, 1, 1, header, extension);
    header.ddspf.fourCC = ddspp::FOURCC_DXT1;
    assert(!ddspp::is_dxt10(header));
    constexpr size_t baseSize = sizeof(ddspp::DDS_MAGIC) + sizeof(header);
    std::vector<uint8_t> dds(baseSize + 8 + 1);
    auto* unaligned = dds.data() + 1;
    std::memcpy(unaligned, &ddspp::DDS_MAGIC, 4);
    std::memcpy(unaligned + 4, &header, sizeof(header));
    ddspp::Descriptor descriptor{};
    assert(DecodeDdsHeader(unaligned, baseSize + 8, descriptor));
    assert(descriptor.width == 1 && descriptor.height == 1 && descriptor.headerSize == baseSize);
    DdsUploadLayout layout;
    assert(BuildDdsUploadLayout(descriptor, baseSize + 8, 256, 512, layout));
    assert(layout.slices.size() == 1 && layout.uploadSize == 512);
    assert(layout.slices[0].srcRowPitch == 8 && layout.slices[0].dstRowPitch == 256 && layout.slices[0].rowWidth == 128);
    assert(!BuildDdsUploadLayout(descriptor, baseSize + 7, 256, 512, layout));
    for (size_t size = 0; size < baseSize; ++size)
        assert(!DecodeDdsHeader(unaligned, size, descriptor));
    assert(!DecodeDdsHeader(nullptr, 1024, descriptor));
    header.ddspf.fourCC = ddspp::FOURCC_DXT10;
    std::memcpy(unaligned + 4, &header, sizeof(header));
    assert(!DecodeDdsHeader(unaligned, baseSize + 8, descriptor));

    descriptor = {};
    descriptor.width = 5;
    descriptor.height = 3;
    descriptor.depth = 1;
    descriptor.numMips = 3;
    descriptor.arraySize = 1;
    descriptor.type = ddspp::Cubemap;
    descriptor.blockWidth = descriptor.blockHeight = 4;
    descriptor.bitsPerPixelOrBlock = 64;
    descriptor.headerSize = ddspp::MAX_HEADER_SIZE;
    assert(BuildDdsUploadLayout(descriptor, descriptor.headerSize + 6 * 32, 256, 512, layout));
    assert(layout.arraySize == 6 && layout.slices.size() == 18 && layout.uploadSize == 18 * 512);
    assert(layout.slices[0].srcRowPitch == 16 && layout.slices[2].srcOffset == 24);
    assert(layout.slices.back().srcOffset == 184 && layout.slices.back().dstOffset == 17 * 512);
    for (size_t size = 0; size < 6 * 32; ++size)
        assert(!BuildDdsUploadLayout(descriptor, descriptor.headerSize + size, 256, 512, layout));
    descriptor.arraySize = UINT32_MAX;
    assert(!BuildDdsUploadLayout(descriptor, SIZE_MAX, 256, 512, layout));

    uint32_t rgbaPitch;
    size_t rgbaSize;
    assert(BuildRgba8UploadFootprint(1, 1, 256, rgbaPitch, rgbaSize));
    assert(rgbaPitch == 256 && rgbaSize == 256);
    assert(BuildRgba8UploadFootprint(16384, 16384, 256, rgbaPitch, rgbaSize));
    assert(rgbaPitch == 65536 && rgbaSize == 1024 * 1024 * 1024);
    if constexpr (sizeof(size_t) >= 8)
    {
        // This previously wrapped a 32-bit upload size to zero. The caller
        // rejects dimensions beyond its GPU limit, and planning remains safe.
        assert(BuildRgba8UploadFootprint(1, 16777216, 256, rgbaPitch, rgbaSize));
        assert(rgbaSize == uint64_t(1) << 32);
    }
    assert(!BuildRgba8UploadFootprint(UINT32_MAX, 1, 256, rgbaPitch, rgbaSize));
    assert(!BuildRgba8UploadFootprint(1, 0, 256, rgbaPitch, rgbaSize));
    assert(!BuildRgba8UploadFootprint(1, 1, 3, rgbaPitch, rgbaSize));
    descriptor.arraySize = 1;
    descriptor.numMips = 33;
    assert(!BuildDdsUploadLayout(descriptor, SIZE_MAX, 256, 512, layout));
    descriptor.numMips = 4; // A 5x3 texture has at most three mip levels.
    assert(!BuildDdsUploadLayout(descriptor, SIZE_MAX, 256, 512, layout));
    descriptor.numMips = 3;
    descriptor.width = UINT32_MAX;
    descriptor.blockWidth = 1;
    descriptor.bitsPerPixelOrBlock = 128;
    assert(!BuildDdsUploadLayout(descriptor, SIZE_MAX, 256, 512, layout));
    descriptor.width = 4;
    descriptor.height = descriptor.depth = 4;
    descriptor.blockHeight = 1;
    descriptor.bitsPerPixelOrBlock = 32;
    descriptor.type = ddspp::Texture3D;
    assert(BuildDdsUploadLayout(descriptor, descriptor.headerSize + 292, 256, 512, layout));
    assert(layout.slices.size() == 3 && layout.uploadSize == 5632);
    assert(layout.slices[1].srcOffset == 256 && layout.slices[2].srcOffset == 288);
    assert(!BuildDdsUploadLayout(descriptor, SIZE_MAX, 0, 512, layout));
    assert(!BuildDdsUploadLayout(descriptor, SIZE_MAX, 256, 3, layout));
    descriptor.depth = 0;
    assert(!BuildDdsUploadLayout(descriptor, SIZE_MAX, 256, 512, layout));

    // Exercise decoder boundaries with arbitrary blocks and small cropped
    // images. Sanitizers verify that neither source nor output is overrun.
    std::mt19937 random(0x360);
    for (unsigned trial = 0; trial < 5000; ++trial)
    {
        std::array<uint8_t, 64> bytes;
        for (auto& byte : bytes) byte = uint8_t(random());
        const auto format = Format(random() % 5);
        const uint32_t width = random() % 9;
        const uint32_t height = random() % 9;
        BlockCompression::DecodeMip(std::span(bytes).first(random() % 65), width, height, format, pixels);
    }

    // Compare valid layouts to the pre-existing copy algorithm for small
    // compressed/uncompressed volumes, arrays and complete mip chains.
    for (unsigned trial = 0; trial < 1000; ++trial)
    {
        descriptor = {};
        descriptor.width = 1 + random() % 33;
        descriptor.height = 1 + random() % 33;
        descriptor.depth = 1 + random() % 5;
        descriptor.arraySize = 1 + random() % 3;
        descriptor.numMips = std::bit_width(std::max({descriptor.width, descriptor.height, descriptor.depth}));
        descriptor.type = ddspp::Texture2D;
        descriptor.blockWidth = descriptor.blockHeight = trial % 3 == 0 ? 1 : 4;
        descriptor.bitsPerPixelOrBlock = trial % 3 == 0 ? 32 : (trial % 3 == 1 ? 64 : 128);
        std::vector<DdsUploadSlice> expected;
        size_t sourceSize = 0, destinationSize = 0;
        for (uint32_t array = 0; array < descriptor.arraySize; ++array)
        {
            for (uint32_t mip = 0; mip < descriptor.numMips; ++mip)
            {
                DdsUploadSlice slice{};
                slice.width = std::max(1u, descriptor.width >> mip);
                slice.height = std::max(1u, descriptor.height >> mip);
                slice.depth = std::max(1u, descriptor.depth >> mip);
                slice.srcOffset = sourceSize;
                slice.dstOffset = destinationSize;
                slice.srcRowPitch = (((slice.width + descriptor.blockWidth - 1) / descriptor.blockWidth) * descriptor.bitsPerPixelOrBlock + 7) / 8;
                slice.dstRowPitch = (slice.srcRowPitch + 255) & ~255u;
                slice.rowCount = (slice.height + descriptor.blockHeight - 1) / descriptor.blockHeight;
                slice.rowWidth = slice.dstRowPitch * 8 / descriptor.bitsPerPixelOrBlock * descriptor.blockWidth;
                sourceSize += slice.srcRowPitch * slice.rowCount * slice.depth;
                destinationSize += (slice.dstRowPitch * slice.rowCount * slice.depth + 511) & ~511u;
                expected.push_back(slice);
            }
        }
        assert(BuildDdsUploadLayout(descriptor, sourceSize, 256, 512, layout));
        assert(layout.uploadSize == destinationSize && layout.slices.size() == expected.size());
        for (size_t index = 0; index < expected.size(); ++index)
        {
            const auto& actual = layout.slices[index];
            const auto& wanted = expected[index];
            assert(actual.width == wanted.width && actual.height == wanted.height && actual.depth == wanted.depth);
            assert(actual.srcOffset == wanted.srcOffset && actual.dstOffset == wanted.dstOffset);
            assert(actual.srcRowPitch == wanted.srcRowPitch && actual.dstRowPitch == wanted.dstRowPitch);
            assert(actual.rowCount == wanted.rowCount && actual.rowWidth == wanted.rowWidth);
        }
        assert(!BuildDdsUploadLayout(descriptor, sourceSize - 1, 256, 512, layout));
    }
    std::cout << "BC decoding and DDS bounds checks passed\n";
}
