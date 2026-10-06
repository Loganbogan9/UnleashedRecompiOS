#pragma once

#include <algorithm>
#include <memory>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <span>

struct VirtualFileSystem {
    using ChunkSink = std::function<bool(std::span<const uint8_t>)>;
    static constexpr size_t StreamChunkSize = 64 * 1024;
    virtual ~VirtualFileSystem() { };
    virtual bool load(const std::string &path, uint8_t *fileData, size_t fileDataMaxByteCount) const = 0;
    virtual size_t getSize(const std::string &path) const = 0;
    virtual bool exists(const std::string &path) const = 0;
    virtual const std::string &getName() const = 0;
    // Deliver a file without allocating a buffer proportional to its size.
    // Returning false from the sink stops reading immediately.
    virtual bool stream(const std::string& path, const ChunkSink& sink) const = 0;

    // Concrete implementation shortcut.
    bool load(const std::string &path, std::vector<uint8_t> &fileData)
    {
        size_t fileDataSize = getSize(path);
        if (fileDataSize == 0)
        {
            return false;
        }

        fileData.resize(fileDataSize);
        return load(path, fileData.data(), fileDataSize);
    }
};
