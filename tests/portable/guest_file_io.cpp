#include <stdafx.h>
#include <kernel/memory.h>
#include <kernel/heap.h>
#include <kernel/xdm.h>
#include <array>
#include <cstdio>
#include <cstdlib>

Memory g_memory;
Heap g_userHeap;
struct FileHandle;
struct FindHandle;
FileHandle* XCreateFileA(const char*, uint32_t, uint32_t, void*, uint32_t, uint32_t);
uint32_t XReadFile(FileHandle*, void*, uint32_t, be<uint32_t>*, XOVERLAPPED*);
uint32_t XWriteFile(FileHandle*, const void*, uint32_t, be<uint32_t>*, void*);
uint32_t XSetFilePointer(FileHandle*, int32_t, be<int32_t>*, uint32_t);
uint32_t XSetFilePointerEx(FileHandle*, int32_t, LARGE_INTEGER*, uint32_t);
FindHandle* XFindFirstFileA(const char*, WIN32_FIND_DATAA*);
uint32_t XFindNextFileA(FindHandle*, WIN32_FIND_DATAA*);

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::abort(); } } while (false)

int main()
{
    CHECK(g_memory.base != nullptr);
    g_userHeap.Init();
    std::string temporary = (std::filesystem::temp_directory_path() / "unleashed-file-io-XXXXXX").string();
    CHECK(mkdtemp(temporary.data()) != nullptr);
    const std::filesystem::path directory(temporary);
    const auto filePath = directory / "save.bin";
    { std::ofstream out(filePath, std::ios::binary); out << "Original"; }
    auto* file = XCreateFileA(filePath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0);
    CHECK(reinterpret_cast<void*>(file) != GetInvalidKernelObject());
    std::array<char, 3> read{};
    be<uint32_t> bytesRead;
    CHECK(XReadFile(file, read.data(), read.size(), &bytesRead, nullptr));
    CHECK(bytesRead == 3);
    be<uint32_t> bytesWritten;
    CHECK(XWriteFile(file, "Sonic", 5, &bytesWritten, nullptr));
    CHECK(bytesWritten == 5); // gcount still describes the preceding 3-byte read
    CHECK(XSetFilePointer(file, 0, nullptr, FILE_BEGIN) == 0);
    std::array<char, 8> out{};
    CHECK(XReadFile(file, out.data(), out.size(), &bytesRead, nullptr));
    CHECK(std::string_view(out.data(), out.size()) == "OriSonic");
    CHECK(XSetFilePointer(file, -1, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER);
    LARGE_INTEGER newPosition{};
    CHECK(!XSetFilePointerEx(file, -1, &newPosition, FILE_BEGIN));
    XOVERLAPPED overlapped{};
    overlapped.Offset = 0xFFFFFFFF;
    overlapped.OffsetHigh = 0xFFFFFFFF;
    CHECK(!XReadFile(file, out.data(), out.size(), nullptr, &overlapped));
    be<int32_t> high = 1;
    CHECK(XSetFilePointer(file, -2, &high, FILE_BEGIN) == 0xFFFFFFFE);
    CHECK(high == 1); // low word is unsigned when a high word is supplied
    high = -1;
    CHECK(XSetFilePointer(file, -1, &high, FILE_BEGIN) == INVALID_SET_FILE_POINTER);
    DestroyKernelObject(reinterpret_cast<KernelObject*>(file));

    // Sparse file tests exercise both file-size halves without allocating gigabytes.
    const auto largePath = directory / "large.bin";
    { std::ofstream out(largePath, std::ios::binary); }
    constexpr uint64_t largeSize = 0x100000123;
    std::filesystem::resize_file(largePath, largeSize);
    WIN32_FIND_DATAA info{};
    const std::string search = directory.string() + "/*";
    auto* find = XFindFirstFileA(search.c_str(), &info);
    CHECK(reinterpret_cast<void*>(find) != GetInvalidKernelObject());
    bool foundSmall = false, foundLarge = false;
    do
    {
        if (std::string_view(info.cFileName) == "save.bin")
        {
            CHECK(ByteSwap(info.nFileSizeLow) == 8);
            CHECK(ByteSwap(info.nFileSizeHigh) == 0);
            foundSmall = true;
        }
        if (std::string_view(info.cFileName) == "large.bin")
        {
            CHECK(ByteSwap(info.nFileSizeLow) == 0x123);
            CHECK(ByteSwap(info.nFileSizeHigh) == 1);
            foundLarge = true;
        }
    } while (XFindNextFileA(find, &info));
    CHECK(foundSmall && foundLarge);
    DestroyKernelObject(reinterpret_cast<KernelObject*>(find));
    CHECK(o1heapDoInvariantsHold(g_userHeap.physicalHeap));
    std::filesystem::remove_all(directory);
    munmap(g_memory.base, PPC_MEMORY_SIZE);
    std::puts("Production guest file I/O regressions passed");
}
