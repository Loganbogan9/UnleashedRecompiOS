#pragma once
#include <filesystem>
#include <vector>
#include <string_view>
struct ModLoader
{
    inline static bool s_isLogTypeConsole = false;
    static const std::vector<std::filesystem::path>* GetIncludeDirectories(size_t) { return nullptr; }
    static std::filesystem::path ResolvePath(std::string_view) { return {}; }
};
