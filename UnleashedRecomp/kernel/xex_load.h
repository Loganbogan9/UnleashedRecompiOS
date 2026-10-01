#pragma once

#include <xex.h>
#include <cstring>
#include <span>
#include <string_view>

namespace xex_load
{
    // Read disk structures without assuming that optional-header offsets are aligned.
    template<class T>
    T Read(std::span<const uint8_t> bytes, size_t offset)
    {
        T value{};
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }

    inline bool Contains(size_t size, size_t offset, size_t length)
    {
        return offset <= size && length <= size - offset;
    }

    struct Image
    {
        uint32_t loadAddress{};
        uint32_t imageSize{};
        uint32_t entryPoint{};
        uint32_t resourceAddress{};
        uint32_t resourceSize{};
        std::span<const uint8_t> data;
        std::span<const uint8_t> basicBlocks;
    };

    // Validate every source and destination extent before modifying guest memory.
    // The installer produces decrypted, uncompressed or basic-compressed XEX files.
    inline bool Validate(std::span<const uint8_t> bytes, uint64_t memorySize, Image& result, std::string_view& error)
    {
        const auto fail = [&](std::string_view message) { error = message; return false; };
        result = {};
        error = {};
        if (bytes.size() < sizeof(Xex2Header))
            return fail("truncated XEX header");

        const auto header = Read<Xex2Header>(bytes, 0);
        if (header.magic != 0x58455832)
            return fail("invalid XEX2 signature");
        const size_t headerSize = header.headerSize;
        if (headerSize < sizeof(header) || headerSize > bytes.size())
            return fail("invalid XEX header size");
        if (header.headerCount > (headerSize - sizeof(header)) / sizeof(Xex2OptHeader))
            return fail("truncated optional-header table");
        if (!Contains(headerSize, header.securityOffset, sizeof(Xex2SecurityInfo)))
            return fail("truncated security header");

        const auto security = Read<Xex2SecurityInfo>(bytes, header.securityOffset);
        Image image;
        image.loadAddress = security.loadAddress;
        image.imageSize = security.imageSize;
        if (image.loadAddress == 0 || image.imageSize == 0 ||
            image.loadAddress > memorySize || image.imageSize > memorySize - image.loadAddress)
            return fail("image exceeds guest memory");

        size_t formatOffset = 0;
        size_t resourceOffset = 0;
        bool hasEntry = false;
        for (size_t i = 0; i < header.headerCount; ++i)
        {
            const auto optional = Read<Xex2OptHeader>(bytes, sizeof(header) + i * sizeof(Xex2OptHeader));
            switch (optional.key.get())
            {
            case XEX_HEADER_FILE_FORMAT_INFO:
                if (formatOffset != 0)
                    return fail("duplicate file-format header");
                formatOffset = optional.offset;
                break;
            case XEX_HEADER_RESOURCE_INFO:
                if (resourceOffset != 0)
                    return fail("duplicate resource header");
                resourceOffset = optional.offset;
                break;
            case XEX_HEADER_ENTRY_POINT:
                if (hasEntry)
                    return fail("duplicate entry point");
                image.entryPoint = optional.value;
                hasEntry = true;
                break;
            }
        }

        if (!hasEntry || (image.entryPoint & 3) != 0 || image.entryPoint < image.loadAddress ||
            uint64_t(image.entryPoint) - image.loadAddress >= image.imageSize)
            return fail("invalid entry point");
        if (formatOffset == 0 || !Contains(headerSize, formatOffset, sizeof(Xex2OptFileFormatInfo)))
            return fail("missing or truncated file-format header");
        if (resourceOffset == 0 || !Contains(headerSize, resourceOffset, sizeof(Xex2ResourceInfo)))
            return fail("missing or truncated resource header");

        const auto format = Read<Xex2OptFileFormatInfo>(bytes, formatOffset);
        const size_t infoSize = format.infoSize;
        if (infoSize < sizeof(format) || !Contains(headerSize, formatOffset, infoSize))
            return fail("invalid file-format header size");
        if (format.encryptionType != XEX_ENCRYPTION_NONE)
            return fail("executable is still encrypted; run the installer");

        const auto resource = Read<Xex2ResourceInfo>(bytes, resourceOffset);
        if (resource.sizeOfHeader < sizeof(resource) || !Contains(headerSize, resourceOffset, resource.sizeOfHeader))
            return fail("invalid resource header size");
        image.resourceAddress = resource.offset;
        image.resourceSize = resource.sizeOfData;
        if (image.resourceAddress < image.loadAddress ||
            !Contains(image.imageSize, uint64_t(image.resourceAddress) - image.loadAddress, image.resourceSize))
            return fail("resource exceeds loaded image");

        image.data = bytes.subspan(headerSize);
        if (format.compressionType == XEX_COMPRESSION_NONE)
        {
            if (image.data.size() < image.imageSize)
                return fail("truncated executable image");
        }
        else if (format.compressionType == XEX_COMPRESSION_BASIC)
        {
            const size_t blockBytes = infoSize - sizeof(format);
            if (blockBytes == 0 || blockBytes % sizeof(Xex2FileBasicCompressionBlock) != 0)
                return fail("invalid basic-compression table");
            image.basicBlocks = bytes.subspan(formatOffset + sizeof(format), blockBytes);
            size_t sourceSize = 0;
            size_t targetSize = 0;
            for (size_t offset = 0; offset < blockBytes; offset += sizeof(Xex2FileBasicCompressionBlock))
            {
                const auto block = Read<Xex2FileBasicCompressionBlock>(image.basicBlocks, offset);
                if (!Contains(image.data.size(), sourceSize, block.dataSize) ||
                    !Contains(image.imageSize, targetSize, block.dataSize))
                    return fail("basic-compression data exceeds image");
                sourceSize += block.dataSize;
                targetSize += block.dataSize;
                if (!Contains(image.imageSize, targetSize, block.zeroSize))
                    return fail("basic-compression zero fill exceeds image");
                targetSize += block.zeroSize;
            }
            if (targetSize != image.imageSize)
                return fail("basic-compression blocks do not cover image");
        }
        else
            return fail("unsupported compression; run the installer");

        result = image;
        return true;
    }
}
