#include "installer.h"

#include <cstring>
#include <array>
#include <cstdio>
#include <memory>
#include <new>
#include <xxh3.h>

#include "directory_file_system.h"
#include "iso_file_system.h"
#include "xcontent_file_system.h"

#if defined(UNLEASHED_RECOMP_IOS)
#include "gpu/astc_cache.h"
#include "gpu/bc_decoder.h"
#include "gpu/dds_header.h"
#include <bc7decomp.h>
#include <plume_render_interface_types.h>
#endif

#include "hashes/apotos_shamar.h"
#include "hashes/chunnan.h"
#include "hashes/empire_city_adabat.h"
#include "hashes/game.h"
#include "hashes/holoska.h"
#include "hashes/mazuri.h"
#include "hashes/spagonia.h"
#include "hashes/update.h"
#include <os/logger.h>
#include <os/macos/ios_diagnostics.h>

static void logInstallerEvent(const char* event, const char* source = "", size_t bytes = 0) noexcept
{
    try
    {
        char message[512];
        const int length = std::snprintf(message, sizeof(message), "Installer %s: %s (%zu bytes)", event, source, bytes);
        if (length > 0)
            os::logger::Log(std::string_view(message, std::min(size_t(length), sizeof(message) - 1)));
        os::logger::LogRuntimeDiagnostics(event);
    }
    catch (...)
    {
        // Diagnostics must not interrupt cleanup when the process is low on memory.
    }
}

static bool reportAllocationFailure(Journal& journal) noexcept
{
    journal.lastResult = Journal::Result::MemoryAllocationFailed;
    constexpr char message[] = "Not enough memory to finish installation. Please retry with fewer DLC packs selected.";
    journal.lastErrorMessage = journal.lastErrorMessage.capacity() >= sizeof(message) - 1 ? message : "Out of memory.";
    logInstallerEvent("allocation failed", journal.activeFile);
    return false;
}

static const std::string GameDirectory = "game";
static const std::string DLCDirectory = "dlc";
static const std::string PatchedDirectory = "patched";
static const std::string ApotosShamarDirectory = DLCDirectory + "/Apotos & Shamar Adventure Pack";
static const std::string ChunnanDirectory = DLCDirectory + "/Chun-nan Adventure Pack";
static const std::string EmpireCityAdabatDirectory = DLCDirectory + "/Empire City & Adabat Adventure Pack";
static const std::string HoloskaDirectory = DLCDirectory + "/Holoska Adventure Pack";
static const std::string MazuriDirectory = DLCDirectory + "/Mazuri Adventure Pack";
static const std::string SpagoniaDirectory = DLCDirectory + "/Spagonia Adventure Pack";
static const std::string UpdateDirectory = "update";
static const std::string GameExecutableFile = "default.xex";
static const std::string DLCValidationFile = "DLC.xml";
static const std::string UpdateExecutablePatchFile = "default.xexp";
static const std::string ISOExtension = ".iso";
static const std::string OldExtension = ".old";
static const std::string TempExtension = ".tmp";

static std::string fromU8(const std::u8string &str)
{
    return std::string(str.begin(), str.end());
}

static std::string fromPath(const std::filesystem::path &path)
{
    return fromU8(path.u8string());
}

static std::string toLower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) { return std::tolower(c); });
    return str;
};

static std::unique_ptr<VirtualFileSystem> createFileSystemFromPath(const std::filesystem::path &path)
{
    try
    {
        if (XContentFileSystem::check(path))
        {
            return XContentFileSystem::create(path);
        }

        if (toLower(fromPath(path.extension())) == ISOExtension)
        {
            return ISOFileSystem::create(path);
        }

        std::error_code ec;
        bool isDirectory = std::filesystem::is_directory(path, ec);
        if (!ec && isDirectory)
        {
            return DirectoryFileSystem::create(path);
        }
    }
    catch (...)
    {
        return nullptr;
    }

    return nullptr;
}

static bool checkFile(const FilePair &pair, const uint64_t *fileHashes, const std::filesystem::path &targetDirectory, std::vector<uint8_t> &fileData, Journal &journal, const std::function<bool()> &progressCallback, bool checkSizeOnly) {
    const std::string fileName(pair.first);
    const uint32_t hashCount = pair.second;
    const std::filesystem::path filePath = targetDirectory / fileName;
    if (!std::filesystem::exists(filePath))
    {
        journal.lastResult = Journal::Result::FileMissing;
        journal.lastErrorMessage = fmt::format("File {} does not exist.", fileName);
        return false;
    }

    std::error_code ec;
    size_t fileSize = std::filesystem::file_size(filePath, ec);
    if (ec)
    {
        journal.lastResult = Journal::Result::FileReadFailed;
        journal.lastErrorMessage = fmt::format("Failed to read file size for {}.", fileName);
        return false;
    }

    if (checkSizeOnly)
    {
        journal.progressTotal += fileSize;
    }
    else
    {
        std::ifstream fileStream(filePath, std::ios::binary);
        if (fileStream.is_open())
        {
            fileData.resize(fileSize);
            fileStream.read((char *)(fileData.data()), fileSize);
        }

        if (!fileStream.is_open() || fileStream.bad())
        {
            journal.lastResult = Journal::Result::FileReadFailed;
            journal.lastErrorMessage = fmt::format("Failed to read file {}.", fileName);
            return false;
        }

        uint64_t fileHash = XXH3_64bits(fileData.data(), fileSize);
        bool fileHashFound = false;
        for (uint32_t i = 0; i < hashCount && !fileHashFound; i++)
        {
            fileHashFound = fileHash == fileHashes[i];
        }

        if (!fileHashFound)
        {
            journal.lastResult = Journal::Result::FileHashFailed;
            journal.lastErrorMessage = fmt::format("File {} did not match any of the known hashes.", fileName);
            return false;
        }

        journal.progressCounter += fileSize;
    }

    if (!progressCallback())
    {
        journal.lastResult = Journal::Result::Cancelled;
        journal.lastErrorMessage = "Check was cancelled.";
        return false;
    }

    return true;
}

#if defined(UNLEASHED_RECOMP_IOS)
static bool BuildASTCCacheForTexture(const std::filesystem::path& cacheRoot, std::span<const uint8_t> fileData)
{
    ddspp::Descriptor descriptor{};
    if (!DecodeDdsHeader(fileData.data(), fileData.size(), descriptor) || descriptor.type != ddspp::Texture2D ||
        descriptor.arraySize != 1 || descriptor.depth != 1 || descriptor.numMips == 0)
        return true;

    const bool bc3 = descriptor.format == ddspp::BC3_UNORM || descriptor.format == ddspp::BC3_UNORM_SRGB;
    const bool bc7 = descriptor.format == ddspp::BC7_UNORM || descriptor.format == ddspp::BC7_UNORM_SRGB;
    if (!bc3 && !bc7)
        return true;

    // Prewarming is optional. Leave large textures to the runtime rather than
    // allocating an unbounded decoded image alongside the installer UI.
    constexpr size_t MaxDecodedBytes = 64 * 1024 * 1024;
    if (descriptor.width == 0 || descriptor.height == 0 ||
        descriptor.width > MaxDecodedBytes / 4 / descriptor.height)
        return true;

    std::vector<uint8_t> rgba;
    const auto payload = fileData.subspan(descriptor.headerSize);
    bool decoded = false;
    if (bc3)
    {
        decoded = BlockCompression::DecodeMip(payload, descriptor.width, descriptor.height,
            BlockCompression::Format::BC3, rgba);
    }
    else
    {
        const size_t blocksX = descriptor.width / 4 + (descriptor.width % 4 != 0);
        const size_t blocksY = descriptor.height / 4 + (descriptor.height % 4 != 0);
        if (blocksX == 0 || blocksY == 0 || blocksX > payload.size() / 16 / blocksY)
            return true;

        rgba.assign(size_t(descriptor.width) * descriptor.height * 4, 0);
        decoded = true;
        for (uint32_t by = 0; by < blocksY && decoded; by++)
        {
            for (uint32_t bx = 0; bx < blocksX; bx++)
            {
                bc7decomp::color_rgba pixels[16];
                decoded = bc7decomp::unpack_bc7(payload.data() + (size_t(by) * blocksX + bx) * 16, pixels);
                if (!decoded)
                    break;
                for (uint32_t py = 0; py < 4; py++)
                {
                    for (uint32_t px = 0; px < 4; px++)
                    {
                        const uint32_t x = bx * 4 + px;
                        const uint32_t y = by * 4 + py;
                        if (x < descriptor.width && y < descriptor.height)
                            std::memcpy(&rgba[(size_t(y) * descriptor.width + x) * 4], &pixels[py * 4 + px], 4);
                    }
                }
            }
        }
    }

    if (!decoded)
        return true;

    const auto cacheFormat = static_cast<uint32_t>(((bc7 && descriptor.format == ddspp::BC7_UNORM_SRGB) ||
        (bc3 && descriptor.format == ddspp::BC3_UNORM_SRGB)) ? plume::RenderFormat::ASTC_6x6_UNORM_SRGB :
        plume::RenderFormat::ASTC_6x6_UNORM);
    std::vector<uint8_t> astc;
    if (EncodeRGBA8ToASTC(rgba, descriptor.width, descriptor.height, 6, 6, astc))
    {
        StoreASTCCache(cacheRoot, HashTextureForASTCCache(fileData.data(), fileData.size()),
            descriptor.width, descriptor.height, cacheFormat, astc);
    }
    return true;
}
#endif

static bool createDirectories(const std::filesystem::path& directory, Journal& journal)
{
    // Record only missing directories, and do so before creating anything.
    // Existing installs must survive cancellation or an allocation failure.
    for (auto path = directory; !path.empty() && !std::filesystem::exists(path); path = path.parent_path())
        journal.createdDirectories.insert(path);
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec)
    {
        journal.lastResult = Journal::Result::DirectoryCreationFailed;
        journal.lastErrorMessage = "Unable to create directory at " + fromPath(directory);
        return false;
    }
    return true;
}

static Journal::FileWrite& prepareFile(const std::filesystem::path& target, Journal& journal)
{
    // Use unused siblings so an interrupted previous run or unrelated .tmp/.old
    // files are never overwritten. Allocate the journal entry before any write.
    for (size_t index = 0; ; ++index)
    {
        auto temporary = target;
        auto backup = target;
        const auto suffix = ".installing." + std::to_string(index);
        temporary += suffix + TempExtension;
        backup += suffix + OldExtension;
        if (!std::filesystem::exists(temporary) && !std::filesystem::exists(backup))
        {
            journal.fileWrites.push_back({target, std::move(temporary), std::move(backup)});
            return journal.fileWrites.back();
        }
    }
}

static bool commitFile(Journal::FileWrite& write, Journal& journal)
{
    std::error_code ec;
    if (std::filesystem::exists(write.target))
    {
        std::filesystem::rename(write.target, write.backup, ec);
        if (!ec)
            write.backedUp = true;
    }
    if (!ec)
    {
        std::filesystem::rename(write.temporary, write.target, ec);
        if (!ec)
            write.installed = true;
    }
    if (ec)
    {
        journal.lastResult = Journal::Result::FileWriteFailed;
        journal.lastErrorMessage = "Failed to replace file at " + fromPath(write.target);
        return false;
    }
    return true;
}

static void finishInstallation(Journal& journal) noexcept
{
    std::error_code ec;
    for (const auto& write : journal.fileWrites)
    {
        try
        {
            if (write.backedUp)
                std::filesystem::remove(write.backup, ec);
        }
        catch (...)
        {
            // A cleanup failure must not roll back a committed installation
            // after some of its backups have already been removed.
        }
    }
    journal.fileWrites.clear();
    journal.createdDirectories.clear();
}

static bool copyFile(const FilePair &pair, const uint64_t *fileHashes, VirtualFileSystem &sourceVfs, const std::filesystem::path &targetDirectory, const std::filesystem::path &cacheRoot, bool skipHashChecks, Journal &journal, const std::function<bool()> &progressCallback)
{
    journal.activeFile = pair.first;
    const std::string filename(pair.first);
    if (!sourceVfs.exists(filename))
    {
        journal.lastResult = Journal::Result::FileMissing;
        journal.lastErrorMessage = fmt::format("File {} does not exist in {}.", filename, sourceVfs.getName());
        return false;
    }
    const auto targetPath = targetDirectory / std::filesystem::path(std::u8string_view((const char8_t*)pair.first));
    if (!createDirectories(targetPath.parent_path(), journal))
        return false;
    auto& write = prepareFile(targetPath, journal);
    std::ofstream output(write.temporary, std::ios::binary);
    if (!output)
    {
        journal.lastResult = Journal::Result::FileCreationFailed;
        journal.lastErrorMessage = "Failed to create file at " + fromPath(targetPath);
        return false;
    }

    std::unique_ptr<XXH3_state_t, decltype(&XXH3_freeState)> hash(nullptr, XXH3_freeState);
    if (!skipHashChecks)
    {
        hash.reset(XXH3_createState());
        if (!hash)
            throw std::bad_alloc();
        XXH3_64bits_reset(hash.get());
    }

    size_t copied = 0;
    bool cancelled = false;
    const bool read = sourceVfs.stream(filename, [&](std::span<const uint8_t> bytes) {
        if (!output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()))
            return false;
        if (hash)
            XXH3_64bits_update(hash.get(), bytes.data(), bytes.size());
        copied += bytes.size();
        journal.progressCounter += bytes.size();
        cancelled = !progressCallback();
        return !cancelled;
    });
    output.close();
    if (cancelled || !read || !output || copied != sourceVfs.getSize(filename))
    {
        journal.lastResult = cancelled ? Journal::Result::Cancelled :
            (!output ? Journal::Result::FileWriteFailed : Journal::Result::FileReadFailed);
        journal.lastErrorMessage = cancelled ? "Installation was cancelled." :
            fmt::format("Failed to copy file {} from {}.", filename, sourceVfs.getName());
        return false;
    }

    if (hash)
    {
        const uint64_t digest = XXH3_64bits_digest(hash.get());
        if (std::find(fileHashes, fileHashes + pair.second, digest) == fileHashes + pair.second)
        {
            journal.lastResult = Journal::Result::FileHashFailed;
            journal.lastErrorMessage = fmt::format("File {} from {} did not match any of the known hashes.", filename, sourceVfs.getName());
            return false;
        }
    }
    // Check cancellation for empty files too, before changing the installed file.
    if (!progressCallback())
    {
        journal.lastResult = Journal::Result::Cancelled;
        journal.lastErrorMessage = "Installation was cancelled.";
        return false;
    }
    if (!commitFile(write, journal))
        return false;

#if defined(UNLEASHED_RECOMP_IOS)
    // Cache generation must not make a successfully copied asset fail to install.
    // Read only modest DDS files, and release their buffers after each texture.
    constexpr size_t MaxCacheSourceBytes = 32 * 1024 * 1024;
    try
    {
        if (copied <= MaxCacheSourceBytes && toLower(fromPath(targetPath.extension())) == ".dds")
        {
            logInstallerEvent("texture cache", filename.c_str(), copied);
            std::vector<uint8_t> texture(copied);
            std::ifstream input(targetPath, std::ios::binary);
            if (input.read(reinterpret_cast<char*>(texture.data()), texture.size()))
                BuildASTCCacheForTexture(cacheRoot, texture);
        }
    }
    catch (const std::bad_alloc&)
    {
        // The original DDS remains valid; the runtime can decode it on demand.
        logInstallerEvent("texture cache skipped after allocation failure", filename.c_str(), copied);
    }
#else
    (void)cacheRoot;
#endif
    return true;
}

static DLC detectDLC(const std::filesystem::path &sourcePath, VirtualFileSystem &sourceVfs, Journal &journal)
{
    // Only the type is needed during source preparation. Scan with a fixed
    // window so a large XML cannot allocate its full size on the UI thread.
    constexpr std::string_view opening = "<Type>";
    constexpr std::string_view closing = "</Type>";
    std::array<char, closing.size()> window{};
    size_t windowSize = 0;
    size_t typeByteCount = 0;
    char typeNumber = 0;
    bool foundOpening = false;
    bool foundClosing = false;
    bool foundNull = false;
    const bool read = sourceVfs.stream(DLCValidationFile, [&](std::span<const uint8_t> bytes) {
        for (const char byte : bytes)
        {
            if (byte == '\0')
            {
                foundNull = true;
                return false;
            }
            if (foundOpening)
            {
                if (typeByteCount == 0)
                    typeNumber = byte;
                ++typeByteCount;
            }
            if (windowSize < window.size())
                window[windowSize++] = byte;
            else
            {
                std::memmove(window.data(), window.data() + 1, window.size() - 1);
                window.back() = byte;
            }
            if (!foundOpening && windowSize >= opening.size() &&
                std::memcmp(window.data() + windowSize - opening.size(), opening.data(), opening.size()) == 0)
            {
                foundOpening = true;
                windowSize = 0;
            }
            else if (foundOpening && windowSize == closing.size() &&
                std::memcmp(window.data(), closing.data(), closing.size()) == 0)
            {
                foundClosing = true;
                typeByteCount -= closing.size();
                return false; // Stop as soon as the type has been read.
            }
        }
        return true;
    });
    if (!read && !foundClosing && !foundNull)
    {
        const std::string sourceName = sourceVfs.getName().empty() ? fromPath(sourcePath.filename()) : sourceVfs.getName();
        if (sourceVfs.exists(DLCValidationFile))
        {
            journal.lastResult = Journal::Result::FileReadFailed;
            journal.lastErrorMessage = fmt::format("Failed to read file {} from {}.", DLCValidationFile, sourceName);
        }
        else
        {
            journal.lastResult = Journal::Result::FileMissing;
            journal.lastErrorMessage = fmt::format("File {} does not exist in {}.", DLCValidationFile, sourceName);
        }
        return DLC::Unknown;
    }

    if (!foundClosing)
    {
        journal.lastResult = Journal::Result::DLCParsingFailed;
        journal.lastErrorMessage = fmt::format("Failed to find DLC type for {}.", sourceVfs.getName());
        return DLC::Unknown;
    }

    if (typeByteCount != 1)
    {
        journal.lastResult = Journal::Result::UnknownDLCType;
        journal.lastErrorMessage = fmt::format("DLC type for {} is unknown.", sourceVfs.getName());
        return DLC::Unknown;
    }

    switch (typeNumber)
    {
    case '1':
        return DLC::Spagonia;
    case '2':
        return DLC::Chunnan;
    case '3':
        return DLC::Mazuri;
    case '4':
        return DLC::Holoska;
    case '5':
        return DLC::ApotosShamar;
    case '7':
        return DLC::EmpireCityAdabat;
    default:
        journal.lastResult = Journal::Result::UnknownDLCType;
        journal.lastErrorMessage = fmt::format("DLC type for {} is unknown.", sourceVfs.getName());
        return DLC::Unknown;
    }
}

static bool fillDLCSource(DLC dlc, Installer::DLCSource &dlcSource) 
{
    switch (dlc)
    {
    case DLC::Spagonia:
        dlcSource.filePairs = { SpagoniaFiles, SpagoniaFilesSize };
        dlcSource.fileHashes = SpagoniaHashes;
        dlcSource.targetSubDirectory = SpagoniaDirectory;
        return true;
    case DLC::Chunnan:
        dlcSource.filePairs = { ChunnanFiles, ChunnanFilesSize };
        dlcSource.fileHashes = ChunnanHashes;
        dlcSource.targetSubDirectory = ChunnanDirectory;
        return true;
    case DLC::Mazuri:
        dlcSource.filePairs = { MazuriFiles, MazuriFilesSize };
        dlcSource.fileHashes = MazuriHashes;
        dlcSource.targetSubDirectory = MazuriDirectory;
        return true;
    case DLC::Holoska:
        dlcSource.filePairs = { HoloskaFiles, HoloskaFilesSize };
        dlcSource.fileHashes = HoloskaHashes;
        dlcSource.targetSubDirectory = HoloskaDirectory;
        return true;
    case DLC::ApotosShamar:
        dlcSource.filePairs = { ApotosShamarFiles, ApotosShamarFilesSize };
        dlcSource.fileHashes = ApotosShamarHashes;
        dlcSource.targetSubDirectory = ApotosShamarDirectory;
        return true;
    case DLC::EmpireCityAdabat:
        dlcSource.filePairs = { EmpireCityAdabatFiles, EmpireCityAdabatFilesSize };
        dlcSource.fileHashes = EmpireCityAdabatHashes;
        dlcSource.targetSubDirectory = EmpireCityAdabatDirectory;
        return true;
    default:
        return false;
    }
}

bool Installer::checkGameInstall(const std::filesystem::path &baseDirectory, std::filesystem::path &modulePath)
{
    modulePath = baseDirectory / PatchedDirectory / GameExecutableFile;

    if (!std::filesystem::exists(modulePath))
        return false;

    if (!std::filesystem::exists(baseDirectory / UpdateDirectory / UpdateExecutablePatchFile))
        return false;

    if (!std::filesystem::exists(baseDirectory / GameDirectory / GameExecutableFile))
        return false;

    return true;
}

bool Installer::checkDLCInstall(const std::filesystem::path &baseDirectory, DLC dlc)
{
    switch (dlc)
    {
    case DLC::Spagonia:
        return std::filesystem::exists(baseDirectory / SpagoniaDirectory / DLCValidationFile);
    case DLC::Chunnan:
        return std::filesystem::exists(baseDirectory / ChunnanDirectory / DLCValidationFile);
    case DLC::Mazuri:
        return std::filesystem::exists(baseDirectory / MazuriDirectory / DLCValidationFile);
    case DLC::Holoska:
        return std::filesystem::exists(baseDirectory / HoloskaDirectory / DLCValidationFile);
    case DLC::ApotosShamar:
        return std::filesystem::exists(baseDirectory / ApotosShamarDirectory / DLCValidationFile);
    case DLC::EmpireCityAdabat:
        return std::filesystem::exists(baseDirectory / EmpireCityAdabatDirectory / DLCValidationFile);
    default:
        return false;
    }
}

bool Installer::checkAllDLC(const std::filesystem::path& baseDirectory)
{
    bool result = true;

    for (int i = 1; i < (int)DLC::Count; i++)
    {
        if (!checkDLCInstall(baseDirectory, (DLC)i))
            result = false;
    }

    return result;
}

bool Installer::checkInstallIntegrity(const std::filesystem::path &baseDirectory, Journal &journal, const std::function<bool()> &progressCallback)
{
    // Run the file checks twice: once to fill out the progress counter and the file sizes, and another pass to do the hash integrity checks.
    for (uint32_t checkPass = 0; checkPass < 2; checkPass++)
    {
        bool checkSizeOnly = (checkPass == 0);
        if (!checkFiles({ GameFiles, GameFilesSize }, GameHashes, baseDirectory / GameDirectory, journal, progressCallback, checkSizeOnly))
        {
            return false;
        }

        if (!checkFiles({ UpdateFiles, UpdateFilesSize }, UpdateHashes, baseDirectory / UpdateDirectory, journal, progressCallback, checkSizeOnly))
        {
            return false;
        }

        for (int i = 1; i < (int)DLC::Count; i++)
        {
            if (checkDLCInstall(baseDirectory, (DLC)i))
            {
                Installer::DLCSource dlcSource;
                fillDLCSource((DLC)i, dlcSource);

                if (!checkFiles(dlcSource.filePairs, dlcSource.fileHashes, baseDirectory / dlcSource.targetSubDirectory, journal, progressCallback, checkSizeOnly))
                {
                    return false;
                }
            }
        }
    }

    return true;
}

bool Installer::computeTotalSize(std::span<const FilePair> filePairs, const uint64_t *fileHashes, VirtualFileSystem &sourceVfs, Journal &journal, uint64_t &totalSize)
{
    for (FilePair pair : filePairs)
    {
        const std::string filename(pair.first);
        if (!sourceVfs.exists(filename))
        {
            journal.lastResult = Journal::Result::FileMissing;
            journal.lastErrorMessage = fmt::format("File {} does not exist in {}.", filename, sourceVfs.getName());
            return false;
        }

        totalSize += sourceVfs.getSize(filename);
    }

    return true;
}

bool Installer::checkFiles(std::span<const FilePair> filePairs, const uint64_t *fileHashes, const std::filesystem::path &targetDirectory, Journal &journal, const std::function<bool()> &progressCallback, bool checkSizeOnly)
{
    FilePair validationPair = {};
    uint32_t validationHashIndex = 0;
    uint32_t hashIndex = 0;
    uint32_t hashCount = 0;
    std::vector<uint8_t> fileData;
    for (FilePair pair : filePairs)
    {
        hashIndex = hashCount;
        hashCount += pair.second;

        if (!checkFile(pair, &fileHashes[hashIndex], targetDirectory, fileData, journal, progressCallback, checkSizeOnly))
        {
            return false;
        }
    }

    return true;
}

bool Installer::copyFiles(std::span<const FilePair> filePairs, const uint64_t *fileHashes, VirtualFileSystem &sourceVfs, const std::filesystem::path &targetDirectory, const std::filesystem::path &cacheRoot, const std::string &validationFile, bool skipHashChecks, Journal &journal, const std::function<bool()> &progressCallback)
{
    if (!createDirectories(targetDirectory, journal))
        return false;

    FilePair validationPair = {};
    uint32_t validationHashIndex = 0;
    uint32_t hashIndex = 0;
    uint32_t hashCount = 0;
    for (FilePair pair : filePairs)
    {
        hashIndex = hashCount;
        hashCount += pair.second;

        if (validationFile.compare(pair.first) == 0)
        {
            validationPair = pair;
            validationHashIndex = hashIndex;
            continue;
        }

        if (!copyFile(pair, &fileHashes[hashIndex], sourceVfs, targetDirectory, cacheRoot, skipHashChecks, journal, progressCallback))
        {
            return false;
        }
    }

    // Validation file is copied last after all other files have been copied.
    if (validationPair.first != nullptr)
    {
        if (!copyFile(validationPair, &fileHashes[validationHashIndex], sourceVfs, targetDirectory, cacheRoot, skipHashChecks, journal, progressCallback))
        {
            return false;
        }
    }
    else
    {
        journal.lastResult = Journal::Result::ValidationFileMissing;
        journal.lastErrorMessage = fmt::format("Unable to find validation file {} in {}.", validationFile, sourceVfs.getName());
        return false;
    }

    return true;
}

bool Installer::parseContent(const std::filesystem::path &sourcePath, std::unique_ptr<VirtualFileSystem> &targetVfs, Journal &journal)
{
    targetVfs = createFileSystemFromPath(sourcePath);
    if (targetVfs != nullptr)
    {
        return true;
    }
    else
    {
        journal.lastResult = Journal::Result::VirtualFileSystemFailed;
        journal.lastErrorMessage = "Unable to open " + fromPath(sourcePath);
        return false;
    }
}

constexpr uint32_t PatcherContribution = 512 * 1024 * 1024;

bool Installer::parseSources(const Input &input, Journal &journal, Sources &sources)
try
{
    journal = Journal();
    sources = Sources();
    journal.lastErrorMessage.reserve(256);
    logInstallerEvent("source preparation (streamed metadata)");

    // Parse the contents of the base game.
    if (!input.gameSource.empty())
    {
        if (!parseContent(input.gameSource, sources.game, journal))
        {
            return false;
        }

        if (!computeTotalSize({ GameFiles, GameFilesSize }, GameHashes, *sources.game, journal, sources.totalSize))
        {
            return false;
        }
    }

    // Parse the contents of Update.
    if (!input.updateSource.empty())
    {
        // Add an arbitrary progress size for the patching process.
        journal.progressTotal += PatcherContribution;

        if (!parseContent(input.updateSource, sources.update, journal))
        {
            return false;
        }

        if (!computeTotalSize({ UpdateFiles, UpdateFilesSize }, UpdateHashes, *sources.update, journal, sources.totalSize))
        {
            return false;
        }
    }

    // Parse the contents of the DLC Packs.
    for (const auto &path : input.dlcSources)
    {
        sources.dlc.emplace_back();
        DLCSource &dlcSource = sources.dlc.back();
        if (!parseContent(path, dlcSource.sourceVfs, journal))
        {
            return false;
        }

        journal.activeFile = DLCValidationFile.c_str();
        DLC dlc = detectDLC(path, *dlcSource.sourceVfs, journal);
        if (!fillDLCSource(dlc, dlcSource))
        {
            return false;
        }

        if (!computeTotalSize(dlcSource.filePairs, dlcSource.fileHashes, *dlcSource.sourceVfs, journal, sources.totalSize))
        {
            return false;
        }
    }

    // Add the total size in bytes as the journal progress.
    journal.progressTotal += sources.totalSize;

    return true;
}
catch (const std::bad_alloc&)
{
    sources = Sources();
    return reportAllocationFailure(journal);
}

bool Installer::install(const Sources &sources, const std::filesystem::path &targetDirectory, bool skipHashChecks, Journal &journal, std::chrono::seconds endWaitTime, const std::function<bool()> &progressCallback)
try
{
    // Reserve an error message before starting work so allocation failures can
    // return to the wizard and run rollback without formatting another message.
    journal.lastErrorMessage.reserve(256);
    logInstallerEvent("start (bounded reads and transactional writes)");
    // Install files in reverse order of importance. In case of a process crash or power outage, this will increase the likelihood of the installation
    // missing critical files required for the game to run. These files are used as the way to detect if the game is installed.

    // Install the DLC.
    for (const DLCSource &dlcSource : sources.dlc)
    {
        logInstallerEvent("DLC pack", dlcSource.sourceVfs->getName().c_str());
        if (!copyFiles(dlcSource.filePairs, dlcSource.fileHashes, *dlcSource.sourceVfs, targetDirectory / dlcSource.targetSubDirectory, targetDirectory, DLCValidationFile, skipHashChecks, journal, progressCallback))
        {
            return false;
        }
    }

    // If no game or update was specified, we're finished. This means the user was only installing the DLC.
    if ((sources.game == nullptr) && (sources.update == nullptr))
    {
        finishInstallation(journal);
        return true;
    }

    // Install the update.
    logInstallerEvent("title update");
    if (!copyFiles({ UpdateFiles, UpdateFilesSize }, UpdateHashes, *sources.update, targetDirectory / UpdateDirectory, targetDirectory, UpdateExecutablePatchFile, skipHashChecks, journal, progressCallback))
    {
        return false;
    }

    // Install the base game.
    logInstallerEvent("base game");
    if (!copyFiles({ GameFiles, GameFilesSize }, GameHashes, *sources.game, targetDirectory / GameDirectory, targetDirectory, GameExecutableFile, skipHashChecks, journal, progressCallback))
    {
        return false;
    }

    // Create the directory where the patched executable will be stored.
    std::filesystem::path patchedDirectory = targetDirectory / PatchedDirectory;
    if (!createDirectories(patchedDirectory, journal))
        return false;

    // Patch the executable with the update's file.
    std::filesystem::path baseXexPath = targetDirectory / GameDirectory / GameExecutableFile;
    std::filesystem::path patchPath = targetDirectory / UpdateDirectory / UpdateExecutablePatchFile;
    std::filesystem::path patchedXexPath = patchedDirectory / GameExecutableFile;
    auto& patchedWrite = prepareFile(patchedXexPath, journal);
    XexPatcher::Result patcherResult = XexPatcher::apply(baseXexPath, patchPath, patchedWrite.temporary);
    if (patcherResult == XexPatcher::Result::Success)
    {
        if (!commitFile(patchedWrite, journal))
            return false;
    }
    else
    {
        journal.lastResult = Journal::Result::PatchProcessFailed;
        journal.lastPatcherResult = patcherResult;
        journal.lastErrorMessage = "Patch process failed.";
        return false;
    }

    // Update the progress with the artificial amount attributed to the patching.
    journal.progressCounter += PatcherContribution;
    
    for (uint32_t i = 0; i < 2; i++)
    {
        if (!progressCallback())
        {
            journal.lastResult = Journal::Result::Cancelled;
            journal.lastErrorMessage = "Installation was cancelled.";
            return false;
        }

        if (i == 0)
        {
            // Wait the specified amount of time to allow the consumer of the callbacks to animate, halt or cancel the installation for a while after it's finished.
            std::this_thread::sleep_for(endWaitTime);
        }
    }

    finishInstallation(journal);
    return true;
}
catch (const std::bad_alloc&)
{
    return reportAllocationFailure(journal);
}
catch (const std::exception&)
{
    journal.lastResult = Journal::Result::UnexpectedError;
    constexpr char message[] = "An unexpected error occurred during installation. Please check the selected files and available storage.";
    journal.lastErrorMessage = journal.lastErrorMessage.capacity() >= sizeof(message) - 1 ? message : "Install failed.";
    return false;
}

void Installer::rollback(Journal &journal)
{
    std::error_code ec;
    for (auto it = journal.fileWrites.rbegin(); it != journal.fileWrites.rend(); ++it)
    {
        if (it->installed)
            std::filesystem::remove(it->target, ec);
        if (it->backedUp)
            std::filesystem::rename(it->backup, it->target, ec);
        std::filesystem::remove(it->temporary, ec);
    }

    for (auto it = journal.createdDirectories.rbegin(); it != journal.createdDirectories.rend(); it++)
    {
        std::filesystem::remove(*it, ec);
    }
    journal.fileWrites.clear();
    journal.createdDirectories.clear();
}

bool Installer::parseGame(const std::filesystem::path &sourcePath)
{
    try
    {
        std::unique_ptr<VirtualFileSystem> sourceVfs = createFileSystemFromPath(sourcePath);
        if (sourceVfs == nullptr)
        {
            return false;
        }

        return sourceVfs->exists(GameExecutableFile);
    }
    catch (...)
    {
        return false;
    }
}

bool Installer::parseUpdate(const std::filesystem::path &sourcePath)
{
    try
    {
        std::unique_ptr<VirtualFileSystem> sourceVfs = createFileSystemFromPath(sourcePath);
        if (sourceVfs == nullptr)
        {
            return false;
        }

        return sourceVfs->exists(UpdateExecutablePatchFile);
    }
    catch (...)
    {
        return false;
    }
}

DLC Installer::parseDLC(const std::filesystem::path &sourcePath)
{
    try
    {
        Journal journal;
        std::unique_ptr<VirtualFileSystem> sourceVfs = createFileSystemFromPath(sourcePath);
        if (sourceVfs == nullptr)
        {
            return DLC::Unknown;
        }

        return detectDLC(sourcePath, *sourceVfs, journal);
    }
    catch (...)
    {
        return DLC::Unknown;
    }
}

XexPatcher::Result Installer::checkGameUpdateCompatibility(const std::filesystem::path &gameSourcePath, const std::filesystem::path &updateSourcePath)
{
    auto pathToString = [](const std::filesystem::path& path)
    {
        std::u8string value = path.u8string();
        return std::string(value.begin(), value.end());
    };

    auto patcherResultToString = [](XexPatcher::Result result)
    {
        switch (result)
        {
            case XexPatcher::Result::Success: return "Success";
            case XexPatcher::Result::FileOpenFailed: return "FileOpenFailed";
            case XexPatcher::Result::FileWriteFailed: return "FileWriteFailed";
            case XexPatcher::Result::XexFileUnsupported: return "XexFileUnsupported";
            case XexPatcher::Result::XexFileInvalid: return "XexFileInvalid";
            case XexPatcher::Result::PatchFileInvalid: return "PatchFileInvalid";
            case XexPatcher::Result::PatchIncompatible: return "PatchIncompatible";
            case XexPatcher::Result::PatchFailed: return "PatchFailed";
            case XexPatcher::Result::PatchUnsupported: return "PatchUnsupported";
            default: return "Unknown";
        }
    };

    std::unique_ptr<VirtualFileSystem> gameSourceVfs = createFileSystemFromPath(gameSourcePath);
    if (gameSourceVfs == nullptr)
    {
        os::logger::Log(fmt::format(
            "Installer::checkGameUpdateCompatibility - failed to create game VFS for '{}'",
            pathToString(gameSourcePath)));
        return XexPatcher::Result::FileOpenFailed;
    }

    std::unique_ptr<VirtualFileSystem> updateSourceVfs = createFileSystemFromPath(updateSourcePath);
    if (updateSourceVfs == nullptr)
    {
        os::logger::Log(fmt::format(
            "Installer::checkGameUpdateCompatibility - failed to create update VFS for '{}'",
            pathToString(updateSourcePath)));
        return XexPatcher::Result::FileOpenFailed;
    }

    std::vector<uint8_t> xexBytes;
    std::vector<uint8_t> patchBytes;
    if (!gameSourceVfs->load(GameExecutableFile, xexBytes))
    {
        os::logger::Log(fmt::format(
            "Installer::checkGameUpdateCompatibility - failed to load '{}' from game source '{}' (vfs='{}', exists={}, size={})",
            GameExecutableFile,
            pathToString(gameSourcePath),
            gameSourceVfs->getName(),
            gameSourceVfs->exists(GameExecutableFile),
            gameSourceVfs->getSize(GameExecutableFile)));
        return XexPatcher::Result::FileOpenFailed;
    }

    if (!updateSourceVfs->load(UpdateExecutablePatchFile, patchBytes))
    {
        os::logger::Log(fmt::format(
            "Installer::checkGameUpdateCompatibility - failed to load '{}' from update source '{}' (vfs='{}', exists={}, size={})",
            UpdateExecutablePatchFile,
            pathToString(updateSourcePath),
            updateSourceVfs->getName(),
            updateSourceVfs->exists(UpdateExecutablePatchFile),
            updateSourceVfs->getSize(UpdateExecutablePatchFile)));
        return XexPatcher::Result::FileOpenFailed;
    }

    std::vector<uint8_t> patchedBytes;
    XexPatcher::Result result = XexPatcher::apply(xexBytes.data(), xexBytes.size(), patchBytes.data(), patchBytes.size(), patchedBytes, true);
    if (result != XexPatcher::Result::Success)
    {
        os::logger::Log(fmt::format(
            "Installer::checkGameUpdateCompatibility - patch apply failed: result={} ({}) game='{}' update='{}' xexBytes={} patchBytes={}",
            int(result),
            patcherResultToString(result),
            pathToString(gameSourcePath),
            pathToString(updateSourcePath),
            xexBytes.size(),
            patchBytes.size()));
    }

    return result;
}
