#pragma once

#include <filesystem>
#include <array>
#include <cstring>
#include <fstream>

#include "virtual_file_system.h"

struct DirectoryFileSystem : VirtualFileSystem
{
    std::filesystem::path directoryPath;
    std::string name;

    DirectoryFileSystem(const std::filesystem::path &directoryPath)
    {
        this->directoryPath = directoryPath;
        name = (const char *)(directoryPath.filename().u8string().data());
    }

    bool load(const std::string &path, uint8_t *fileData, size_t fileDataMaxByteCount) const override
    {
        const auto size = getSize(path);
        if (size > fileDataMaxByteCount || (size != 0 && fileData == nullptr))
            return false;
        size_t offset = 0;
        const bool loaded = stream(path, [&](std::span<const uint8_t> bytes) {
            if (bytes.size() > size - offset)
                return false;
            std::memcpy(fileData + offset, bytes.data(), bytes.size());
            offset += bytes.size();
            return true;
        });
        return loaded && offset == size;
    }

    size_t getSize(const std::string &path) const override
    {
        std::error_code ec;
        size_t fileSize = std::filesystem::file_size(directoryPath / std::filesystem::path(std::u8string_view((const char8_t *)(path.c_str()))), ec);
        if (!ec)
        {
            return fileSize;
        }
        else
        {
            return 0;
        }
    }

    bool exists(const std::string &path) const override
    {
        if (path.empty())
        {
            return false;
        }

        std::error_code ec;
        bool result = std::filesystem::exists(directoryPath / std::filesystem::path(std::u8string_view((const char8_t *)(path.c_str()))), ec);
        return !ec && result;
    }

    bool stream(const std::string& path, const ChunkSink& sink) const override
    {
        const auto sourcePath = directoryPath / std::filesystem::path(std::u8string_view((const char8_t*)path.c_str()));
        std::error_code ec;
        const auto size = std::filesystem::file_size(sourcePath, ec);
        if (ec || size > SIZE_MAX)
            return false;
        std::ifstream file(sourcePath, std::ios::binary);
        if (!file)
            return false;
        std::array<uint8_t, StreamChunkSize> buffer;
        size_t remaining = size;
        while (remaining != 0)
        {
            const size_t count = std::min(remaining, buffer.size());
            if (!file.read(reinterpret_cast<char*>(buffer.data()), count) ||
                !sink(std::span(buffer).first(count)))
                return false;
            remaining -= count;
        }
        return true;
    }

    const std::string &getName() const override
    {
        return name;
    }

    static std::unique_ptr<VirtualFileSystem> create(const std::filesystem::path &directoryPath)
    {
        std::error_code ec;
        bool exists = std::filesystem::exists(directoryPath, ec);
        bool isDirectory = std::filesystem::is_directory(directoryPath, ec);
        if (!ec && exists && isDirectory)
        {
            return std::make_unique<DirectoryFileSystem>(directoryPath);
        }
        else
        {
            return nullptr;
        }
    }
};
