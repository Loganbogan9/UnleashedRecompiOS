#pragma once

#include <filesystem>
#include <string_view>

struct FileSystem
{
    static std::filesystem::path ResolvePath(const std::string_view& path, bool checkForMods);
};
