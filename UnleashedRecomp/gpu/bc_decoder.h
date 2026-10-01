#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

// Portable decoder used when iOS cannot sample the original BC texture.
// BC2/BC3 color blocks always use four colors, regardless of endpoint order.
namespace BlockCompression
{
enum class Format { BC1, BC2, BC3, BC4, BC5 };

inline uint8_t Expand5To8(uint32_t value)
{
    return uint8_t((value << 3) | (value >> 2));
}

inline uint8_t Expand6To8(uint32_t value)
{
    return uint8_t((value << 2) | (value >> 4));
}

inline void DecodeBC1ColorBlock(const uint8_t* block, uint8_t outRGBA[16 * 4], bool forceFourColors = false)
{
    uint16_t c0 = uint16_t(block[0] | (uint16_t(block[1]) << 8));
    uint16_t c1 = uint16_t(block[2] | (uint16_t(block[3]) << 8));

    uint8_t colors[4][4] = {};

    colors[0][0] = Expand5To8((c0 >> 11) & 0x1F);
    colors[0][1] = Expand6To8((c0 >> 5) & 0x3F);
    colors[0][2] = Expand5To8(c0 & 0x1F);
    colors[0][3] = 255;

    colors[1][0] = Expand5To8((c1 >> 11) & 0x1F);
    colors[1][1] = Expand6To8((c1 >> 5) & 0x3F);
    colors[1][2] = Expand5To8(c1 & 0x1F);
    colors[1][3] = 255;

    if (forceFourColors || c0 > c1)
    {
        for (uint32_t i = 0; i < 3; i++)
        {
            colors[2][i] = uint8_t((2 * colors[0][i] + colors[1][i]) / 3);
            colors[3][i] = uint8_t((colors[0][i] + 2 * colors[1][i]) / 3);
        }
        colors[2][3] = 255;
        colors[3][3] = 255;
    }
    else
    {
        for (uint32_t i = 0; i < 3; i++)
            colors[2][i] = uint8_t((colors[0][i] + colors[1][i]) / 2);

        colors[2][3] = 255;
        colors[3][0] = 0;
        colors[3][1] = 0;
        colors[3][2] = 0;
        colors[3][3] = 0;
    }

    uint32_t indices = uint32_t(block[4]) | (uint32_t(block[5]) << 8) | (uint32_t(block[6]) << 16) | (uint32_t(block[7]) << 24);

    for (uint32_t pixel = 0; pixel < 16; pixel++)
    {
        uint32_t colorIndex = (indices >> (pixel * 2)) & 0x3;
        uint8_t* dst = outRGBA + (pixel * 4);
        dst[0] = colors[colorIndex][0];
        dst[1] = colors[colorIndex][1];
        dst[2] = colors[colorIndex][2];
        dst[3] = colors[colorIndex][3];
    }
}

inline void DecodeBC4Block(const uint8_t* block, uint8_t outValues[16])
{
    uint8_t v0 = block[0];
    uint8_t v1 = block[1];

    uint8_t table[8] = {};
    table[0] = v0;
    table[1] = v1;

    if (v0 > v1)
    {
        table[2] = uint8_t((6 * v0 + 1 * v1) / 7);
        table[3] = uint8_t((5 * v0 + 2 * v1) / 7);
        table[4] = uint8_t((4 * v0 + 3 * v1) / 7);
        table[5] = uint8_t((3 * v0 + 4 * v1) / 7);
        table[6] = uint8_t((2 * v0 + 5 * v1) / 7);
        table[7] = uint8_t((1 * v0 + 6 * v1) / 7);
    }
    else
    {
        table[2] = uint8_t((4 * v0 + 1 * v1) / 5);
        table[3] = uint8_t((3 * v0 + 2 * v1) / 5);
        table[4] = uint8_t((2 * v0 + 3 * v1) / 5);
        table[5] = uint8_t((1 * v0 + 4 * v1) / 5);
        table[6] = 0;
        table[7] = 255;
    }

    uint64_t indices = 0;
    for (uint32_t i = 0; i < 6; i++)
        indices |= (uint64_t(block[2 + i]) << (8 * i));

    for (uint32_t p = 0; p < 16; p++)
        outValues[p] = table[(indices >> (3 * p)) & 0x7];
}

inline bool DecodeMip(std::span<const uint8_t> data, uint32_t width, uint32_t height, Format format, std::vector<uint8_t>& outRGBA)
{
    if (width == 0 || height == 0)
        return false;

    const size_t blocksX = width / 4 + (width % 4 != 0);
    const size_t blocksY = height / 4 + (height % 4 != 0);
    const size_t blockSize = format == Format::BC1 || format == Format::BC4 ? 8 : 16;
    if (blocksX > data.size() / blockSize / blocksY)
        return false;
    if (size_t(width) > std::numeric_limits<size_t>::max() / height / 4)
        return false;
    const size_t outputSize = size_t(width) * height * 4;
    if (outputSize > outRGBA.max_size())
        return false;

    outRGBA.assign(outputSize, 0);
    const uint8_t* src = data.data();

    uint8_t rgbaBlock[16 * 4] = {};
    uint8_t alphaBlock[16] = {};
    uint8_t greenBlock[16] = {};

    for (uint32_t by = 0; by < blocksY; by++)
    {
        for (uint32_t bx = 0; bx < blocksX; bx++)
        {
            const uint8_t* block = src + size_t(by * blocksX + bx) * blockSize;

            if (format == Format::BC1)
            {
                DecodeBC1ColorBlock(block, rgbaBlock);
            }
            else if (format == Format::BC2)
            {
                DecodeBC1ColorBlock(block + 8, rgbaBlock, true);

                for (uint32_t p = 0; p < 16; p++)
                {
                    uint8_t packed = block[p / 2];
                    uint8_t a4 = (p & 1) ? (packed >> 4) : (packed & 0x0F);
                    rgbaBlock[p * 4 + 3] = uint8_t((a4 << 4) | a4);
                }
            }
            else if (format == Format::BC3)
            {
                DecodeBC4Block(block, alphaBlock);

                DecodeBC1ColorBlock(block + 8, rgbaBlock, true);
                for (uint32_t p = 0; p < 16; p++)
                    rgbaBlock[p * 4 + 3] = alphaBlock[p];
            }
            else if (format == Format::BC4)
            {
                DecodeBC4Block(block, alphaBlock);
                for (uint32_t p = 0; p < 16; p++)
                {
                    rgbaBlock[p * 4 + 0] = alphaBlock[p];
                    rgbaBlock[p * 4 + 1] = 0;
                    rgbaBlock[p * 4 + 2] = 0;
                    rgbaBlock[p * 4 + 3] = 255;
                }
            }
            else if (format == Format::BC5)
            {
                DecodeBC4Block(block, alphaBlock);
                DecodeBC4Block(block + 8, greenBlock);

                for (uint32_t p = 0; p < 16; p++)
                {
                    rgbaBlock[p * 4 + 0] = alphaBlock[p];
                    rgbaBlock[p * 4 + 1] = greenBlock[p];
                    rgbaBlock[p * 4 + 2] = 0;
                    rgbaBlock[p * 4 + 3] = 255;
                }
            }
            else
            {
                return false;
            }

            for (uint32_t py = 0; py < 4; py++)
            {
                for (uint32_t px = 0; px < 4; px++)
                {
                    uint32_t x = bx * 4 + px;
                    uint32_t y = by * 4 + py;
                    if (x >= width || y >= height)
                        continue;

                    size_t dstPixel = (size_t(y) * size_t(width) + x) * 4;
                    size_t srcPixel = (size_t(py) * 4 + px) * 4;
                    memcpy(&outRGBA[dstPixel], &rgbaBlock[srcPixel], 4);
                }
            }
        }
    }

    return true;
}

}
