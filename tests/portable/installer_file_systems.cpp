#include <install/iso_file_system.h>
#include <install/xcontent_file_system.h>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { std::cerr << __FILE__ << ':' << __LINE__ << ": " << #condition << '\n'; std::abort(); } } while (false)

// Force the ISO's stream fallback without relying on host memory pressure.
// Only this regression target is linked with --wrap=mmap.
static bool failFileMapping = false;
extern "C" void* __real_mmap(void*, size_t, int, int, int, off_t);
extern "C" void* __wrap_mmap(void* address, size_t length, int protection, int flags, int fd, off_t offset)
{
    if (failFileMapping)
    {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return __real_mmap(address, length, protection, flags, fd, offset);
}

static void putLE(std::vector<uint8_t>& bytes, size_t offset, uint64_t value, size_t count)
{
    CHECK(offset <= bytes.size() && count <= bytes.size() - offset);
    for (size_t i = 0; i < count; ++i)
        bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
static void putBE(std::vector<uint8_t>& bytes, size_t offset, uint64_t value, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        putLE(bytes, offset + i, value >> (8 * (count - 1 - i)), 1);
}
static void write(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    CHECK(out.good());
}
static void directoryEntry(std::vector<uint8_t>& bytes, size_t offset, uint32_t block, uint32_t length,
    const char* name, uint8_t attributes = 0)
{
    putLE(bytes, offset + 4, block, 4);
    putLE(bytes, offset + 8, length, 4);
    bytes[offset + 12] = attributes;
    bytes[offset + 13] = std::strlen(name);
    std::memcpy(bytes.data() + offset + 14, name, std::strlen(name));
}
static std::vector<uint8_t> isoImage()
{
    std::vector<uint8_t> bytes(35 * 2048);
    std::memcpy(bytes.data() + 32 * 2048, "MICROSOFT*XBOX*MEDIA", 20);
    putLE(bytes, 32 * 2048 + 20, 33, 4);
    putLE(bytes, 32 * 2048 + 24, 64, 4);
    directoryEntry(bytes, 33 * 2048, 34, 5, "default.xex");
    std::memcpy(bytes.data() + 34 * 2048, "Sonic", 5);
    return bytes;
}
static std::vector<uint8_t> contentHeader(bool svod)
{
    std::vector<uint8_t> bytes(svod ? 0x1000 : 0x4000);
    std::memcpy(bytes.data(), "LIVE", 4);
    putBE(bytes, 0x340, 0x1000, 4);
    bytes[0x379] = 0x24;
    if (svod)
    {
        bytes[0x391] = 0x40; // enhanced GDF layout
        putBE(bytes, 0x3A9, 1, 4);
    }
    else
    {
        bytes[0x37B] = 1; // read-only STFS
        putLE(bytes, 0x37C, 1, 2); // one directory table block
        std::memcpy(bytes.data() + 0x2000, "default.xex", 11);
        bytes[0x2028] = 11;
        putLE(bytes, 0x202C, 1, 3); // allocated blocks
        putLE(bytes, 0x202F, 1, 3); // start block
        putBE(bytes, 0x2032, 0xFFFF, 2); // root directory
        putBE(bytes, 0x2034, 5, 4);
        std::memcpy(bytes.data() + 0x3000, "Sonic", 5);
    }
    return bytes;
}
static std::vector<uint8_t> svodImage()
{
    std::vector<uint8_t> bytes(0x5000);
    std::memcpy(bytes.data() + 0x2000, "MICROSOFT*XBOX*MEDIA", 20);
    putLE(bytes, 0x2014, 0, 4); // enhanced GDF root block => file offset 0x3000
    directoryEntry(bytes, 0x3000, 1, 5, "default.xex");
    std::memcpy(bytes.data() + 0x3800, "Sonic", 5);
    return bytes;
}
static void checkFile(VirtualFileSystem& fs)
{
    CHECK(fs.exists("default.xex"));
    CHECK(fs.getSize("default.xex") == 5);
    std::array<uint8_t, 8> out{};
    CHECK(fs.load("default.xex", out.data(), out.size()));
    CHECK(std::memcmp(out.data(), "Sonic", 5) == 0);
    CHECK(!fs.load("default.xex", out.data(), 4));
    CHECK(!fs.load("default.xex", nullptr, 5));
    CHECK(!fs.load("missing", out.data(), out.size()));
}

int main()
{
    std::string temporary = (std::filesystem::temp_directory_path() / "unleashed-installer-XXXXXX").string();
    CHECK(mkdtemp(temporary.data()) != nullptr);
    const std::filesystem::path directory(temporary);
    const auto isoPath = directory / "game.iso";
    const auto packagePath = directory / "game.live";
    const auto dataDirectory = std::filesystem::path(packagePath.string() + ".data");
    std::filesystem::create_directory(dataDirectory);
    const auto dataPath = dataDirectory / "Data0000";
    auto iso = isoImage();
    write(isoPath, iso);
    { ISOFileSystem fs(isoPath); CHECK(!fs.empty()); checkFile(fs); }
    failFileMapping = true;
    {
        ISOFileSystem fs(isoPath);
        CHECK(!fs.mappedFile.isOpen());
        checkFile(fs);
        std::filesystem::resize_file(isoPath, 34 * 2048 + 2);
        uint8_t out[5]{};
        CHECK(!fs.load("default.xex", out, sizeof(out)));
    }
    failFileMapping = false;
    write(isoPath, iso);
    putLE(iso, 32 * 2048 + 24, 15, 4); // file name exceeds declared table size
    write(isoPath, iso);
    CHECK(ISOFileSystem::create(isoPath) == nullptr);
    iso = isoImage();
    putLE(iso, 33 * 2048, 8, 2);
    directoryEntry(iso, 33 * 2048 + 32, 34, 5, "loop");
    putLE(iso, 33 * 2048 + 32, 8, 2); // self-referencing sibling
    write(isoPath, iso);
    CHECK(ISOFileSystem::create(isoPath) == nullptr);
    iso = isoImage();
    directoryEntry(iso, 33 * 2048, 33, 64, "loop", 0x10); // directory references itself
    write(isoPath, iso);
    CHECK(ISOFileSystem::create(isoPath) == nullptr);
    iso = isoImage();
    putLE(iso, 32 * 2048 + 20, 0xFFFFFFFF, 4);
    write(isoPath, iso);
    CHECK(ISOFileSystem::create(isoPath) == nullptr);

    auto stfs = contentHeader(false);
    write(packagePath, stfs);
    { XContentFileSystem fs(packagePath); CHECK(!fs.empty()); checkFile(fs); }
    stfs[0x2028] = 41; // length exceeds 40-byte name field
    write(packagePath, stfs);
    CHECK(XContentFileSystem::create(packagePath) == nullptr);
    stfs = contentHeader(false);
    putBE(stfs, 0x2032, 123, 2); // parent does not exist
    write(packagePath, stfs);
    CHECK(XContentFileSystem::create(packagePath) == nullptr);
    stfs = contentHeader(false);
    putBE(stfs, 0x340, 0xFFFFF800, 4); // header alignment must not wrap to zero
    write(packagePath, stfs);
    CHECK(XContentFileSystem::create(packagePath) == nullptr);
    stfs = contentHeader(false);
    putLE(stfs, 0x37C, 2, 2);
    putBE(stfs, 0x1014, 0, 4); // directory hash links table block to itself
    write(packagePath, stfs);
    CHECK(XContentFileSystem::create(packagePath) == nullptr);
    stfs = contentHeader(false);
    putBE(stfs, 0x2034, 0x2000, 4); // chain ends before declared file length
    putLE(stfs, 0x202C, 2, 3);
    putBE(stfs, 0x102C, 0xFFFFFF, 4); // hash entry for data block 1
    write(packagePath, stfs);
    { XContentFileSystem fs(packagePath); std::array<uint8_t, 0x2000> out{}; CHECK(!fs.load("default.xex", out.data(), out.size())); }

    write(packagePath, contentHeader(true));
    auto svod = svodImage();
    write(dataPath, svod);
    { XContentFileSystem fs(packagePath); CHECK(!fs.empty()); checkFile(fs); }
    directoryEntry(svod, 0x3000, 0, 32, "loop", 0x10);
    write(dataPath, svod);
    CHECK(XContentFileSystem::create(packagePath) == nullptr);
    svod = svodImage();
    putLE(svod, 0x3000, 8, 2);
    directoryEntry(svod, 0x3020, 1, 5, "loop");
    putLE(svod, 0x3020, 8, 2);
    write(dataPath, svod);
    CHECK(XContentFileSystem::create(packagePath) == nullptr);
    for (size_t length : { 0x2001, 0x2013, 0x2017 })
    {
        svod = svodImage();
        svod.resize(length); // partial magic or missing root block
        write(dataPath, svod);
        CHECK(XContentFileSystem::create(packagePath) == nullptr);
    }
    svod = svodImage();
    putLE(svod, 0x2014, 0xFFFFFFFF, 4); // invalid data file reference
    write(dataPath, svod);
    CHECK(XContentFileSystem::create(packagePath) == nullptr);
    std::filesystem::remove_all(directory);
    std::cout << "Production ISO/STFS/SVOD parser regressions passed\n";
}
