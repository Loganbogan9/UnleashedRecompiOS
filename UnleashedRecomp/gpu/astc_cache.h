#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include <astcenc.h>

struct ASTCCacheHeader
{
    uint32_t magic = 0x31435341;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint64_t sourceHash = 0;
    uint64_t payloadSize = 0;
};

inline uint64_t HashTextureForASTCCache(const uint8_t* data, size_t size)
{
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; i++)
    {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

inline std::filesystem::path ASTCCachePath(const std::filesystem::path& root, uint64_t hash)
{
    return root / "astc_cache" / (std::to_string(hash) + ".astc");
}

inline bool LoadASTCCache(const std::filesystem::path& root, uint64_t hash, uint32_t width, uint32_t height,
    uint32_t format, std::vector<uint8_t>& outData)
{
    const auto path = ASTCCachePath(root, hash);
    std::error_code ec;
    const auto fileSize = std::filesystem::file_size(path, ec);
    if (ec || fileSize < sizeof(ASTCCacheHeader))
        return false;

    ASTCCacheHeader header{};
    std::ifstream file(path, std::ios::binary);
    if (!file || !file.read(reinterpret_cast<char*>(&header), sizeof(header)) ||
        header.magic != ASTCCacheHeader{}.magic || header.width != width || header.height != height ||
        header.format != format || header.sourceHash != hash ||
        header.payloadSize != fileSize - sizeof(header) || header.payloadSize == 0)
        return false;

    outData.resize(size_t(header.payloadSize));
    return file.read(reinterpret_cast<char*>(outData.data()), static_cast<std::streamsize>(outData.size())).good();
}

inline void StoreASTCCache(const std::filesystem::path& root, uint64_t hash, uint32_t width, uint32_t height,
    uint32_t format, std::span<const uint8_t> data)
{
    if (data.empty())
        return;

    const auto directory = root / "astc_cache";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec)
        return;

    const auto path = ASTCCachePath(root, hash);
    const auto temporaryPath = path.string() + ".tmp";
    ASTCCacheHeader header;
    header.width = width;
    header.height = height;
    header.format = format;
    header.sourceHash = hash;
    header.payloadSize = data.size();

    {
        std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!file || !file.write(reinterpret_cast<const char*>(&header), sizeof(header)) ||
            !file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size())))
            return;
    }

    std::filesystem::rename(temporaryPath, path, ec);
    if (ec)
        std::filesystem::remove(temporaryPath, ec);
}

inline bool EncodeRGBA8ToASTC(std::span<const uint8_t> rgba, uint32_t width, uint32_t height,
    uint32_t blockWidth, uint32_t blockHeight, std::vector<uint8_t>& outASTC)
{
    if (width == 0 || height == 0 || rgba.size() != size_t(width) * height * 4)
        return false;

    astcenc_config config{};
    if (astcenc_config_init(ASTCENC_PRF_LDR, blockWidth, blockHeight, 1, ASTCENC_PRE_FAST,
        ASTCENC_FLG_USE_DECODE_UNORM8, &config) != ASTCENC_SUCCESS)
        return false;

    astcenc_context* context = nullptr;
    if (astcenc_context_alloc(&config, 1, &context, nullptr) != ASTCENC_SUCCESS)
        return false;

    void* imageData = const_cast<uint8_t*>(rgba.data());
    astcenc_image image{width, height, 1, ASTCENC_TYPE_U8, &imageData};
    astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    const size_t blocksX = width / blockWidth + (width % blockWidth != 0);
    const size_t blocksY = height / blockHeight + (height % blockHeight != 0);
    outASTC.resize(blocksX * blocksY * 16);
    const auto result = astcenc_compress_image(context, &image, &swizzle, outASTC.data(), outASTC.size(), 0);
    astcenc_context_free(context);
    if (result != ASTCENC_SUCCESS)
    {
        outASTC.clear();
        return false;
    }
    return true;
}
