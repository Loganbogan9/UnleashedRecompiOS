#pragma once

#include <cstring>
#include <fstream>
#include <limits>
#include <memory_mapped_file.h>

// iOS streams packages from the start, leaving address space for texture
// conversion and the installer UI. Other platforms retain the mapped fast path
// with a bounded stream fallback when a package cannot be mapped.
class SourceFile
{
    MemoryMappedFile mapping;
    std::filesystem::path path;
    size_t byteSize = 0;

public:
    explicit SourceFile(const std::filesystem::path& sourcePath) : path(sourcePath)
    {
#if !defined(UNLEASHED_RECOMP_IOS)
        if (mapping.open(path))
        {
            byteSize = mapping.size();
            return;
        }
#endif

        std::ifstream input(path, std::ios::binary | std::ios::ate);
        const auto length = input.tellg();
        if (input.is_open() && length > 0 && uintmax_t(length) <= std::numeric_limits<size_t>::max())
            byteSize = static_cast<size_t>(length);
    }

    bool isOpen() const { return byteSize != 0; }

    // Each parse/load owns its stream position, so const package reads can run
    // independently. Open once per operation, rather than once per 4 KiB block.
    class Reader
    {
        const uint8_t* data;
        size_t byteSize;
        mutable std::ifstream stream;

    public:
        explicit Reader(const SourceFile& file)
            : data(file.mapping.isOpen() ? file.mapping.data() : nullptr), byteSize(file.byteSize)
        {
            if (data == nullptr)
                stream.open(file.path, std::ios::binary);
        }

        bool read(size_t offset, void* output, size_t length) const
        {
            if (offset > byteSize || length > byteSize - offset || (length != 0 && output == nullptr))
                return false;
            if (length == 0)
                return true;
            if (data != nullptr)
            {
                std::memcpy(output, data + offset, length);
                return true;
            }
            if (!stream.is_open() || offset > static_cast<size_t>(std::numeric_limits<std::streamoff>::max())
                || length > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()))
                return false;
            stream.clear();
            stream.seekg(static_cast<std::streamoff>(offset));
            if (!stream.good())
                return false;
            stream.read(static_cast<char*>(output), static_cast<std::streamsize>(length));
            return !stream.bad() && static_cast<size_t>(stream.gcount()) == length;
        }
    };
};
