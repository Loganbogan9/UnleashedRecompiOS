#include <install/installer.h>
#include <install/directory_file_system.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { std::cerr << __FILE__ << ':' << __LINE__ << ": " << #condition << '\n'; std::abort(); } } while (false)

// Compile the actual installer, substituting only proprietary hash tables and
// the unrelated executable patcher. DLC copy/hash/transaction code is production.
#define HASH_TABLE(name, validation) \
    extern const uint64_t name##Hashes[] = {0}; \
    extern const FilePair name##Files[] = {{validation, 1}}; \
    extern const size_t name##FilesSize = 1
HASH_TABLE(Game, "unused");
HASH_TABLE(Update, "unused");
HASH_TABLE(Spagonia, "DLC.xml");
HASH_TABLE(Chunnan, "DLC.xml");
HASH_TABLE(Mazuri, "DLC.xml");
HASH_TABLE(Holoska, "DLC.xml");
HASH_TABLE(ApotosShamar, "DLC.xml");
HASH_TABLE(EmpireCityAdabat, "DLC.xml");
XexPatcher::Result XexPatcher::apply(const uint8_t*, size_t, const uint8_t*, size_t, std::vector<uint8_t>&, bool)
{ return Result::PatchFailed; }
XexPatcher::Result XexPatcher::apply(const std::filesystem::path&, const std::filesystem::path&, const std::filesystem::path&)
{ return Result::PatchFailed; }

// Refuse any single large C++ allocation while copying six packs. A whole-file
// vector for the 384 KiB fixture would fail; bounded production copying succeeds.
static size_t allocationLimit = std::numeric_limits<size_t>::max();
static bool failNextAllocation = false;
static size_t allocationsUntilFailure = std::numeric_limits<size_t>::max();
void* operator new(size_t size)
{
    if (std::exchange(failNextAllocation, false) || size > allocationLimit)
        throw std::bad_alloc();
    if (allocationsUntilFailure == 0)
    {
        allocationsUntilFailure = std::numeric_limits<size_t>::max();
        throw std::bad_alloc();
    }
    if (allocationsUntilFailure != std::numeric_limits<size_t>::max())
        --allocationsUntilFailure;
    if (auto pointer = std::malloc(std::max(size, size_t(1))))
        return pointer;
    throw std::bad_alloc();
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, size_t) noexcept { std::free(pointer); }

static void write(const std::filesystem::path& path, std::span<const uint8_t> bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    CHECK(file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}
static void write(const std::filesystem::path& path, const std::string& text)
{
    write(path, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}
static std::vector<uint8_t> read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::vector<uint8_t> bytes(std::filesystem::file_size(path));
    CHECK(file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()));
    return bytes;
}
static std::string readText(const std::filesystem::path& path)
{
    const auto bytes = read(path);
    return {bytes.begin(), bytes.end()};
}

enum class Failure { None, ShortRead, Allocation, Unexpected };
struct Source : DirectoryFileSystem
{
    Failure failure;
    Source(const std::filesystem::path& path, Failure failure) : DirectoryFileSystem(path), failure(failure) { }
    bool load(const std::string&, uint8_t*, size_t) const override
    {
        CHECK(false); // Installation must use bounded streaming, never load().
        return false;
    }
    bool stream(const std::string& path, const ChunkSink& sink) const override
    {
        return DirectoryFileSystem::stream(path, [&](std::span<const uint8_t> bytes) {
            CHECK(bytes.size() <= StreamChunkSize);
            if (!sink(bytes))
                return false;
            if (path == "nested/asset.bin")
            {
                if (failure == Failure::Allocation)
                    throw std::bad_alloc();
                if (failure == Failure::Unexpected)
                    throw std::runtime_error("Injected source error");
                if (failure == Failure::ShortRead)
                    return false;
            }
            return true;
        });
    }
};

static const FilePair files[] = {{"DLC.xml", 1}, {"nested/asset.bin", 2}};
static Installer::Sources sources(const std::filesystem::path& path, const uint64_t* hashes, Failure failure = Failure::None)
{
    Installer::Sources result;
    for (int index = 0; index < 6; ++index)
    {
        auto& pack = result.dlc.emplace_back();
        pack.sourceVfs = std::make_unique<Source>(path, index == 1 ? failure : Failure::None);
        pack.filePairs = files;
        pack.fileHashes = hashes;
        pack.targetSubDirectory = "dlc/pack" + std::to_string(index);
    }
    return result;
}

int main()
{
    std::string temporary = (std::filesystem::temp_directory_path() / "unleashed-copy-XXXXXX").string();
    CHECK(mkdtemp(temporary.data()) != nullptr);
    const std::filesystem::path root(temporary);
    const auto sourcePath = root / "source";
    std::vector<uint8_t> payload(384 * 1024 + 7);
    for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = uint8_t(i * 17);
    const std::string xml = "<DLC><Type>1</Type></DLC>";
    write(sourcePath / "nested/asset.bin", payload);
    write(sourcePath / "DLC.xml", xml);
    const uint64_t hashes[] = {XXH3_64bits(xml.data(), xml.size()), 0, XXH3_64bits(payload.data(), payload.size())};

    {
        // Type markers can cross read boundaries, and XML bodies can exceed the
        // allocation budget. Production selection still uses constant memory.
        Installer::Input input;
        const char types[] = {'1', '2', '3', '4', '5', '7'};
        for (size_t index = 0; index < std::size(types); ++index)
        {
            const auto pack = root / ("selection" + std::to_string(index));
            const auto openingOffset = VirtualFileSystem::StreamChunkSize * (index + 1) - 3;
            std::string largeXml(openingOffset, ' ');
            largeXml += std::string("<Type>") + types[index] + "</Type>";
            largeXml.append(256 * 1024, ' ');
            write(pack / "DLC.xml", largeXml);
            allocationLimit = 128 * 1024;
            const auto type = Installer::parseDLC(pack);
            allocationLimit = std::numeric_limits<size_t>::max();
            CHECK(type == DLC(index + 1));
            input.dlcSources.push_back(pack);
        }
        Journal journal;
        Installer::Sources selected;
        allocationLimit = 128 * 1024;
        const bool parsed = Installer::parseSources(input, journal, selected);
        allocationLimit = std::numeric_limits<size_t>::max();
        CHECK(parsed && selected.dlc.size() == 6);

        failNextAllocation = true;
        CHECK(!Installer::parseSources(input, journal, selected));
        CHECK(journal.lastResult == Journal::Result::MemoryAllocationFailed);
        CHECK(selected.dlc.empty());
        CHECK(!journal.lastErrorMessage.empty());

        for (const std::string invalid : {"<Type>9</Type>", "<Type>12</Type>", "<Type></Type>", "<Type>1", "no type"})
        {
            write(root / "invalid-selection/DLC.xml", invalid);
            CHECK(Installer::parseDLC(root / "invalid-selection") == DLC::Unknown);
        }
    }

    {
        auto input = sources(sourcePath, hashes);
        Journal journal;
        const auto destination = root / "success";
        allocationLimit = 128 * 1024;
        const bool installed = Installer::install(input, destination, false, journal, std::chrono::seconds(0), [] { return true; });
        allocationLimit = std::numeric_limits<size_t>::max();
        CHECK(installed);
        CHECK(journal.progressCounter == 6 * (payload.size() + xml.size()));
        CHECK(journal.fileWrites.empty());
        for (int index = 0; index < 6; ++index)
        {
            const auto pack = destination / ("dlc/pack" + std::to_string(index));
            CHECK(read(pack / "nested/asset.bin") == payload);
            CHECK(readText(pack / "DLC.xml") == xml);
        }
        for (const auto& entry : std::filesystem::recursive_directory_iterator(destination))
            CHECK(entry.path().filename().string().find(".installing.") == std::string::npos);
    }

    for (const auto failure : {Failure::ShortRead, Failure::Allocation, Failure::Unexpected})
    {
        const auto destination = root / ("failure" + std::to_string(int(failure)));
        const auto priorPack = destination / "dlc/pack0";
        write(priorPack / "nested/asset.bin", "Previously installed asset");
        write(priorPack / "DLC.xml", "Previously installed validation");
        write(priorPack / "nested/asset.bin.installing.0.tmp", "Unrelated temporary file");
        write(destination / "dlc/unselected/DLC.xml", "Unselected pack");
        auto input = sources(sourcePath, hashes, failure);
        Journal journal;
        CHECK(!Installer::install(input, destination, false, journal, std::chrono::seconds(0), [] { return true; }));
        CHECK(journal.lastResult == (failure == Failure::Allocation ? Journal::Result::MemoryAllocationFailed :
            failure == Failure::Unexpected ? Journal::Result::UnexpectedError : Journal::Result::FileReadFailed));
        CHECK(!journal.lastErrorMessage.empty());
        // First pack had been fully replaced when the second pack failed.
        CHECK(read(priorPack / "nested/asset.bin") == payload);
        Installer::rollback(journal);
        CHECK(readText(priorPack / "nested/asset.bin") == "Previously installed asset");
        CHECK(readText(priorPack / "DLC.xml") == "Previously installed validation");
        CHECK(readText(priorPack / "nested/asset.bin.installing.0.tmp") == "Unrelated temporary file");
        CHECK(readText(destination / "dlc/unselected/DLC.xml") == "Unselected pack");
        CHECK(!std::filesystem::exists(destination / "dlc/pack1"));
        CHECK(journal.fileWrites.empty());
        Installer::rollback(journal); // Cleanup is safe to repeat.
    }

    {
        auto input = sources(sourcePath, hashes);
        Journal journal;
        const auto destination = root / "cancelled";
        int callbacks = 0;
        CHECK(!Installer::install(input, destination, false, journal, std::chrono::seconds(0), [&] { return ++callbacks < 2; }));
        CHECK(journal.lastResult == Journal::Result::Cancelled);
        CHECK(callbacks == 2);
        CHECK(!std::filesystem::exists(destination / "dlc/pack0/DLC.xml"));
        Installer::rollback(journal);
        CHECK(!std::filesystem::exists(destination));
    }
    {
        // Fail each allocation in turn, including journal growth and file
        // replacement. Every failed install must restore the previous pack.
        const std::string smallAsset = "Asset for allocation fault injection";
        write(sourcePath / "nested/asset.bin", smallAsset);
        const uint64_t smallHashes[] = {hashes[0], 0, XXH3_64bits(smallAsset.data(), smallAsset.size())};
        const auto destination = root / "allocation-faults";
        const auto priorPack = destination / "dlc/pack0";
        size_t cases = 0;
        for (size_t fault = 0; fault < 2000; ++fault)
        {
            write(priorPack / "nested/asset.bin", "Previously installed asset");
            write(priorPack / "DLC.xml", "Previously installed validation");
            auto input = sources(sourcePath, smallHashes);
            input.dlc.resize(2);
            Journal journal;
            const std::function<bool()> progress = [] { return true; };
            allocationsUntilFailure = fault;
            const bool installed = Installer::install(input, destination, false, journal, std::chrono::seconds(0), progress);
            const bool injected = allocationsUntilFailure == std::numeric_limits<size_t>::max();
            allocationsUntilFailure = std::numeric_limits<size_t>::max();
            if (!installed)
            {
                Installer::rollback(journal);
                CHECK(readText(priorPack / "nested/asset.bin") == "Previously installed asset");
                CHECK(readText(priorPack / "DLC.xml") == "Previously installed validation");
                CHECK(!std::filesystem::exists(destination / "dlc/pack1"));
            }
            else
            {
                CHECK(readText(priorPack / "nested/asset.bin") == smallAsset);
                CHECK(readText(priorPack / "DLC.xml") == xml);
                CHECK(readText(destination / "dlc/pack1/nested/asset.bin") == smallAsset);
            }
            for (const auto& entry : std::filesystem::recursive_directory_iterator(destination))
                CHECK(entry.path().filename().string().find(".installing.") == std::string::npos);
            std::filesystem::remove_all(destination);
            ++cases;
            if (!injected)
                break;
        }
        CHECK(cases > 20 && cases < 2000);
        std::cout << "Verified " << cases << " allocation failure positions\n";
    }
    {
        const uint64_t badHashes[] = {hashes[0], 0, 1};
        auto input = sources(sourcePath, badHashes);
        Journal journal;
        const auto destination = root / "bad-hash";
        CHECK(!Installer::install(input, destination, false, journal, std::chrono::seconds(0), [] { return true; }));
        CHECK(journal.lastResult == Journal::Result::FileHashFailed);
        CHECK(!std::filesystem::exists(destination / "dlc/pack0/nested/asset.bin"));
        Installer::rollback(journal);
        CHECK(!std::filesystem::exists(destination));
    }
    {
        auto input = sources(sourcePath, hashes);
        Journal journal;
        const auto destination = root / "early-allocation-failure";
        const std::function<bool()> progress = [] { return true; };
        failNextAllocation = true;
        CHECK(!Installer::install(input, destination, false, journal, std::chrono::seconds(0), progress));
        CHECK(journal.lastResult == Journal::Result::MemoryAllocationFailed);
        CHECK(!journal.lastErrorMessage.empty());
        Installer::rollback(journal);
        CHECK(!std::filesystem::exists(destination));
    }
    std::filesystem::remove_all(root);
    std::cout << "Production bounded copy/hash, six-pack install, cancellation, and rollback passed\n";
}
