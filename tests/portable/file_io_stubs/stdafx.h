#pragma once
#include "../runtime_stubs/stdafx.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
namespace ankerl::unordered_dense
{
    template<typename K, typename V> using map = std::unordered_map<K, V>;
}
