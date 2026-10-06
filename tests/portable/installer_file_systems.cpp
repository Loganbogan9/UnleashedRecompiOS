#include <install/iso_file_system.h>
#include <install/xcontent_file_system.h>
#include <install/directory_file_system.h>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { std::cerr << __FILE__ << ':' << __LINE__ << ": " << #condition << '\n'; std::abort(); } } while (false)

// Force stream fallbacks without relying on host memory pressure.
// Only this target's memory_mapped_file.cpp calls InstallerTestMmap.
static bool failFileMapping = false;
static size_t mappingsRemaining = std::numeric_limits<size_t>::max();
static size_t mappingsRefused = 0;
static size_t mappingsAttempted = 0;
extern "C" void* InstallerTestMmap(void* address, size_t length, int protection, int flags, int fd, off_t offset)
{
    ++mappingsAttempted;
    if (failFileMapping || mappingsRemaining == 0)
    {
        ++mappingsRefused;
        errno = ENOMEM;
        return MAP_FAILED;
    }
    if (mappingsRemaining != std::numeric_limits<size_t>::max())
        --mappingsRemaining;
    return mmap(address, length, protection, flags, fd, offset);
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
    size_t streamed = 0;
    CHECK(fs.stream("default.xex", [&](std::span<const uint8_t> bytes) {
        CHECK(bytes.size() <= VirtualFileSystem::StreamChunkSize);
        CHECK(streamed <= 5 && bytes.size() <= 5 - streamed);
        CHECK(std::memcmp(bytes.data(), &"Sonic"[streamed], bytes.size()) == 0);
        streamed += bytes.size();
        return true;
    }));
    CHECK(streamed == 5);
    int calls = 0;
    CHECK(!fs.stream("default.xex", [&](std::span<const uint8_t>) { ++calls; return false; }));
    CHECK(calls == 1);
    CHECK(!fs.stream("missing", [](std::span<const uint8_t>) { CHECK(false); return true; }));
}

static void checkDLCBatch(const std::filesystem::path& directory)
{
    const auto gamePath = directory / "batch.iso";
    const auto updatePath = directory / "update.live";
    write(gamePath, isoImage());
    write(updatePath, contentHeader(false));
    std::array<std::string, 6> xml;
    std::array<std::filesystem::path, 6> paths;
    const char types[] = {'1', '2', '3', '4', '5', '7'};
    for (size_t i = 0; i < paths.size(); ++i)
    {
        xml[i] = std::string("<DLC><Type>") + types[i] + "</Type></DLC>";
        paths[i] = directory / ("DLC-" + std::to_string(i) + ".live");
        auto bytes = contentHeader(false);
        std::memset(bytes.data() + 0x2000, 0, 40);
        std::memcpy(bytes.data() + 0x2000, "DLC.xml", 7);
        bytes[0x2028] = 7;
        putBE(bytes, 0x2034, xml[i].size(), 4);
        std::memcpy(bytes.data() + 0x3000, xml[i].data(), xml[i].size());
        write(paths[i], bytes);
    }

    // Hold the game, update and all six DLC simultaneously. Rotate the order
    // so each pack is the one that exceeds the simulated mapping budget.
    for (size_t start = 0; start < paths.size(); ++start)
    {
        mappingsRemaining = 7; // Game, update, and five DLC fit; the sixth cannot map.
        const auto refusedBefore = mappingsRefused;
        auto game = ISOFileSystem::create(gamePath);
        auto update = XContentFileSystem::create(updatePath);
        CHECK(game && update);
        std::array<std::unique_ptr<XContentFileSystem>, 6> packs;
        for (size_t offset = 0; offset < paths.size(); ++offset)
        {
            const size_t index = (start + offset) % paths.size();
            packs[index] = XContentFileSystem::create(paths[index]);
            CHECK(packs[index] != nullptr);
        }
#if defined(UNLEASHED_RECOMP_IOS)
        CHECK(mappingsRefused == refusedBefore);
#else
        CHECK(mappingsRefused == refusedBefore + 1);
#endif
        for (size_t i = 0; i < packs.size(); ++i)
        {
            std::vector<uint8_t> data;
            CHECK(static_cast<VirtualFileSystem&>(*packs[i]).load("DLC.xml", data));
            CHECK(std::string(data.begin(), data.end()) == xml[i]);
            CHECK(packs[i]->getName() == paths[i].filename().string());
        }
        checkFile(*game);
        checkFile(*update);
    }
    mappingsRemaining = std::numeric_limits<size_t>::max();
}

static void checkFragmentedSTFS(const std::filesystem::path& path)
{
    std::vector<uint8_t> payload(9000);
    for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<uint8_t>(i * 17 + i / 4096);
    auto bytes = contentHeader(false);
    bytes.resize(0x7000);
    putLE(bytes, 0x202C, 3, 3);
    putBE(bytes, 0x2034, payload.size(), 4);
    putBE(bytes, 0x102C, 4, 4); // Data chain: blocks 1 -> 4 -> 2.
    putBE(bytes, 0x1074, 2, 4);
    putBE(bytes, 0x1044, 0xFFFFFF, 4);
    std::memcpy(bytes.data() + 0x3000, payload.data(), 4096);
    std::memcpy(bytes.data() + 0x6000, payload.data() + 4096, 4096);
    std::memcpy(bytes.data() + 0x4000, payload.data() + 8192, payload.size() - 8192);
    for (bool streamed : {false, true})
    {
        write(path, bytes);
        failFileMapping = streamed;
        auto fs = XContentFileSystem::create(path);
        CHECK(fs != nullptr);
        auto read = [&] {
            for (int i = 0; i < 10; ++i)
            {
                std::vector<uint8_t> actual(payload.size());
                CHECK(fs->load("default.xex", actual.data(), actual.size()));
                CHECK(actual == payload);
                actual.clear();
                CHECK(fs->stream("default.xex", [&](std::span<const uint8_t> chunk) {
                    CHECK(chunk.size() <= VirtualFileSystem::StreamChunkSize);
                    actual.insert(actual.end(), chunk.begin(), chunk.end());
                    return true;
                }));
                CHECK(actual == payload);
            }
        };
        std::thread otherReader(read);
        read();
        otherReader.join();
        if (streamed)
        {
            std::filesystem::resize_file(path, 0x4010);
            std::vector<uint8_t> actual(payload.size());
            CHECK(!fs->load("default.xex", actual.data(), actual.size()));
            CHECK(!fs->stream("default.xex", [](std::span<const uint8_t>) { return true; }));
        }
    }
    failFileMapping = false;
}

int main()
{
    std::string temporary = (std::filesystem::temp_directory_path() / "unleashed-installer-XXXXXX").string();
    CHECK(mkdtemp(temporary.data()) != nullptr);
    const std::filesystem::path directory(temporary);
    {
        std::vector<uint8_t> payload(VirtualFileSystem::StreamChunkSize * 3 + 7, 0xA5);
        write(directory / "large.bin", payload);
        DirectoryFileSystem fs(directory);
        std::vector<uint8_t> actual;
        CHECK(fs.stream("large.bin", [&](std::span<const uint8_t> bytes) {
            CHECK(bytes.size() <= VirtualFileSystem::StreamChunkSize);
            actual.insert(actual.end(), bytes.begin(), bytes.end());
            return true;
        }));
        CHECK(actual == payload);
        int calls = 0;
        CHECK(!fs.stream("large.bin", [&](std::span<const uint8_t>) { ++calls; return false; }));
        CHECK(calls == 1);
    }
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
        CHECK(!fs.stream("default.xex", [](std::span<const uint8_t>) { return true; }));
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
    failFileMapping = true;
    {
        auto fs = XContentFileSystem::create(packagePath);
        CHECK(fs != nullptr);
        CHECK(fs->getName() == packagePath.filename().string());
        checkFile(*fs);
    }
    failFileMapping = false;
    checkDLCBatch(directory);
    checkFragmentedSTFS(directory / "fragmented.live");
    CHECK(XContentFileSystem::create(directory / "missing.live") == nullptr);
    const auto emptyPath = directory / "empty.live";
    write(emptyPath, {});
    CHECK(XContentFileSystem::create(emptyPath) == nullptr);
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
    failFileMapping = true;
    {
        auto fs = XContentFileSystem::create(packagePath);
        CHECK(fs != nullptr);
        checkFile(*fs);
        std::filesystem::resize_file(dataPath, 0x3802);
        uint8_t out[5]{};
        CHECK(!fs->load("default.xex", out, sizeof(out)));
        CHECK(!fs->stream("default.xex", [](std::span<const uint8_t>) { return true; }));
    }
    failFileMapping = false;
    write(dataPath, svod);
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
#if defined(UNLEASHED_RECOMP_IOS)
    CHECK(mappingsAttempted == 0);
#endif
    std::cout << "Production ISO/STFS/SVOD parser regressions passed\n";
}
