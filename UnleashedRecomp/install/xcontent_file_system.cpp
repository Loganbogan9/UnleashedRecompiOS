// Referenced from: https://github.com/xenia-canary/xenia-canary/blob/canary_experimental/src/xenia/vfs/devices/xcontent_container_device.cc

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */


#include "xcontent_file_system.h"
#include "xbox.h"

#include <bit>
#include <array>
#include <set>
#include <stack>
#include <cstring>
#include <fstream>
#include <optional>
#include <unordered_set>

template<typename T>
static T readBigEndian(const void* address)
{
    // Packed container metadata is not necessarily aligned for be<T>'s member
    // functions. Copy into a host-aligned value before converting endianness.
    T value;
    std::memcpy(&value, address, sizeof(value));
    return ByteSwap(value);
}

enum class XContentPackageType
{
    CON = 0x434F4E20,
    PIRS = 0x50495253,
    LIVE = 0x4C495645,
};

struct XContentLicense
{
    uint64_t licenseId;
    uint32_t licenseBits;
    uint32_t licenseFlags;
};

// Keep on-disk big-endian fields as raw integers. Packed offsets may be
// unaligned, so decode with readBigEndian rather than invoking be<T> members.
#pragma pack(push, 1)
struct XContentHeader
{
    uint32_t magic;
    uint8_t signature[0x228];
    XContentLicense licenses[0x10];
    uint8_t contentId[0x14];
    uint32_t headerSize;
};
static_assert(sizeof(XContentHeader) == 0x344);

struct StfsVolumeDescriptor
{
    uint8_t descriptorLength;
    uint8_t version;

    union
    {
        uint8_t asByte;
        struct
        {
            uint8_t readOnlyFormat : 1;
            uint8_t rootActiveIndex : 1;
            uint8_t directoryOverallocated : 1;
            uint8_t directoryIndexBoundsValid : 1;
        } bits;
    } flags;

    uint16_t fileTableBlockCount;
    uint8_t fileTableBlockNumberRaw[3];
    uint8_t topHashTableHash[0x14];
    uint32_t totalBlockCount;
    uint32_t freeBlockCount;
};
static_assert(sizeof(StfsVolumeDescriptor) == 0x24);

struct StfsDirectoryEntry {
    char name[40];

    struct
    {
        uint8_t nameLength : 6;
        uint8_t contiguous : 1;
        uint8_t directory : 1;
    } flags;

    uint8_t validDataBlocksRaw[3];
    uint8_t allocatedDataBlocksRaw[3];
    uint8_t startBlockNumberRaw[3];
    uint16_t directoryIndex;
    uint32_t length;
    uint16_t createDate;
    uint16_t createTime;
    uint16_t modifiedDate;
    uint16_t modifiedTime;
};
static_assert(sizeof(StfsDirectoryEntry) == 0x40);

struct StfsDirectoryBlock {
    StfsDirectoryEntry entries[0x40];
};
static_assert(sizeof(StfsDirectoryBlock) == 0x1000);

struct StfsHashEntry {
    uint8_t sha1[0x14];
    uint32_t infoRaw;
};
static_assert(sizeof(StfsHashEntry) == 0x18);

struct StfsHashTable {
    StfsHashEntry entries[170];
    uint32_t numBlocks;
    uint8_t padding[12];
};
static_assert(sizeof(StfsHashTable) == 0x1000);

struct SvodDeviceDescriptor {
    uint8_t descriptorLength;
    uint8_t blockCacheElementCount;
    uint8_t workerThreadProcessor;
    uint8_t workerThreadPriority;
    uint8_t firstFragmentHashEntry[0x14];
    union {
        uint8_t asByte;
        struct {
            uint8_t mustBeZeroForFutureUsage : 6;
            uint8_t enhancedGdfLayout : 1;
            uint8_t zeroForDownlevelClients : 1;
        } bits;
    } features;
    uint8_t numDataBlocksRaw[3];
    uint8_t startDataBlockRaw[3];
    uint8_t reserved[5];
};
static_assert(sizeof(SvodDeviceDescriptor) == 0x24);

struct SvodDirectoryEntry {
    uint16_t nodeL;
    uint16_t nodeR;
    uint32_t dataBlock;
    uint32_t length;
    uint8_t attributes;
    uint8_t nameLength;
};
static_assert(sizeof(SvodDirectoryEntry) == 0xE);

struct XContentMetadata
{
    uint32_t contentType;
    uint32_t metadataVersion;
    uint64_t contentSize;
    uint8_t executionInfo[24];
    uint8_t consoleId[5];
    uint64_t profileId;

    union {
        StfsVolumeDescriptor stfsVolumeDescriptor;
        SvodDeviceDescriptor svodDeviceDescriptor;
    };

    uint32_t dataFileCount;
    uint64_t dataFileSize;
    uint32_t volumeType;
    uint64_t onlineCreator;
    uint32_t category;
};
static_assert(sizeof(XContentMetadata) == 0x75);

#pragma pack(pop)

struct XContentContainerHeader
{
    XContentHeader contentHeader;
    XContentMetadata contentMetadata;
};

const uint32_t StfsBlockSize = 0x1000;
const uint32_t StfsBlocksHashLevelAmount = 3;
const uint32_t StfsBlocksPerHashLevel[StfsBlocksHashLevelAmount] = { 170, 28900, 4913000 };
const uint32_t StfsEndOfChain = 0xFFFFFF;
const uint32_t StfsEntriesPerDirectoryBlock = StfsBlockSize / sizeof(StfsDirectoryEntry);

uint32_t parseUint24(const uint8_t *bytes) {
    return bytes[0] | (bytes[1] << 8U) | (bytes[2] << 16U);
}

size_t blockIndexToOffset(uint64_t baseOffset, uint64_t blockIndex)
{
    uint64_t block = blockIndex;
    for (uint32_t i = 0; i < StfsBlocksHashLevelAmount; i++)
    {
        uint32_t levelBase = StfsBlocksPerHashLevel[i];
        block += ((blockIndex + levelBase) / levelBase);
        if (blockIndex < levelBase)
        {
            break;
        }
    }

    return baseOffset + (block << 12);
}

uint32_t blockIndexToHashBlockNumber(uint32_t blockIndex) {
    if (blockIndex < StfsBlocksPerHashLevel[0])
    {
        return 0;
    }

    uint32_t block = (blockIndex / StfsBlocksPerHashLevel[0]) * (StfsBlocksPerHashLevel[0] + 1);
    block += ((blockIndex / StfsBlocksPerHashLevel[1]) + 1);
    if (blockIndex < StfsBlocksPerHashLevel[1])
    {
        return block;
    }

    return block + 1;
}

size_t blockIndexToHashBlockOffset(uint64_t baseOffset, uint32_t blockIndex)
{
    size_t blockNumber = blockIndexToHashBlockNumber(blockIndex);
    return baseOffset + (blockNumber << 12);
}

static bool readHashEntry(const SourceFile::Reader& file, uint64_t baseOffset, uint64_t blockIndex, StfsHashEntry& entry)
{
    size_t hashOffset = blockIndexToHashBlockOffset(baseOffset, blockIndex);
    const size_t entryOffset = hashOffset + (blockIndex % StfsBlocksPerHashLevel[0]) * sizeof(StfsHashEntry);
    return file.read(entryOffset, &entry, sizeof(entry));
}

void blockToOffsetAndFile(SvodLayoutType svodLayoutType, size_t svodStartDataBlock, size_t svodBaseOffset, size_t block, size_t &outOffset, size_t &outFileIndex)
{
    const size_t BlockSize = 0x800;
    const size_t HashBlockSize = 0x1000;
    const size_t BlocksPerL0Hash = 0x198;
    const size_t HashesPerL1Hash = 0xA1C4;
    const size_t BlocksPerFile = 0x14388;
    const size_t MaxFileSize = 0xA290000;
    size_t trueBlock = block - (svodStartDataBlock * 2);
    if (svodLayoutType == SvodLayoutType::EnhancedGDF)
    {
        trueBlock += 0x2;
    }

    size_t fileBlock = trueBlock % BlocksPerFile;
    outFileIndex = trueBlock / BlocksPerFile;

    size_t offset = 0;
    size_t level0TableCount = (fileBlock / BlocksPerL0Hash) + 1;
    offset += level0TableCount * HashBlockSize;

    size_t level1TableCount = (level0TableCount / HashesPerL1Hash) + 1;
    offset += level1TableCount * HashBlockSize;

    if (svodLayoutType == SvodLayoutType::SingleFile)
    {
        offset += svodBaseOffset;
    }

    outOffset = (fileBlock * BlockSize) + offset;
    if (outOffset >= MaxFileSize)
    {
        outOffset = (outOffset % MaxFileSize) + 0x2000;
        outFileIndex++;
    }
}

XContentFileSystem::XContentFileSystem(const std::filesystem::path &contentPath)
{
    decltype(fileMap) parsedFiles;
    name = (const char *)(contentPath.filename().u8string().data());
    sourceFiles.emplace_back(contentPath);
    if (!sourceFiles.back().isOpen())
    {
        sourceFiles.clear();
        return;
    }

    const SourceFile::Reader rootFile(sourceFiles.back());
    XContentContainerHeader contentContainerHeader{};
    if (!rootFile.read(0, &contentContainerHeader, sizeof(contentContainerHeader)))
    {
        sourceFiles.clear();
        return;
    }

    XContentPackageType packageType = XContentPackageType(readBigEndian<uint32_t>(&contentContainerHeader.contentHeader.magic));
    if (packageType != XContentPackageType::CON && packageType != XContentPackageType::LIVE && packageType != XContentPackageType::PIRS)
    {
        sourceFiles.clear();
        return;
    }

    const XContentMetadata &metadata = contentContainerHeader.contentMetadata;
    volumeType = XContentVolumeType(readBigEndian<uint32_t>(&metadata.volumeType));
    if (volumeType == XContentVolumeType::STFS)
    {
        const StfsVolumeDescriptor &descriptor = metadata.stfsVolumeDescriptor;
        if (descriptor.descriptorLength != sizeof(StfsVolumeDescriptor) || !descriptor.flags.bits.readOnlyFormat)
        {
            sourceFiles.clear();
            return;
        }

        baseOffset = ((uint64_t(readBigEndian<uint32_t>(&contentContainerHeader.contentHeader.headerSize)) + StfsBlockSize - 1) / StfsBlockSize) * StfsBlockSize;

        uint32_t entryCount = 0;
        uint32_t tableBlockIndex = parseUint24(descriptor.fileTableBlockNumberRaw);
        uint32_t tableBlockCount = descriptor.fileTableBlockCount;
        std::map<uint32_t, std::string> directoryNames;
        std::unordered_set<uint32_t> visitedTableBlocks;
        for (uint32_t i = 0; i < tableBlockCount; i++)
        {
            size_t offset = blockIndexToOffset(baseOffset, tableBlockIndex);
            StfsDirectoryBlock directoryBlock{};
            if (!rootFile.read(offset, &directoryBlock, sizeof(directoryBlock))
                || !visitedTableBlocks.insert(tableBlockIndex).second)
            {
                sourceFiles.clear();
                return;
            }

            for (uint32_t j = 0; j < StfsEntriesPerDirectoryBlock; j++)
            {
                const StfsDirectoryEntry &directoryEntry = directoryBlock.entries[j];
                if (directoryEntry.name[0] == '\0')
                {
                    break;
                }

                const uint16_t parentIndex = readBigEndian<uint16_t>(&directoryEntry.directoryIndex);
                if (directoryEntry.flags.nameLength == 0 || directoryEntry.flags.nameLength > sizeof(directoryEntry.name)
                    || (parentIndex != 0xFFFF && !directoryNames.contains(parentIndex)))
                {
                    sourceFiles.clear();
                    return;
                }

                std::string fileNameBase = parentIndex == 0xFFFF ? "" : directoryNames.at(parentIndex);
                std::string fileName(directoryEntry.name, directoryEntry.flags.nameLength & 0x3F);
                if (directoryEntry.flags.directory)
                {
                    directoryNames[entryCount++] = fileNameBase + fileName + "/";
                    continue;
                }

                uint32_t fileBlockIndex = parseUint24(directoryEntry.startBlockNumberRaw);
                uint32_t fileBlockCount = parseUint24(directoryEntry.allocatedDataBlocksRaw);
                parsedFiles[fileNameBase + fileName] = { readBigEndian<uint32_t>(&directoryEntry.length), fileBlockIndex, fileBlockCount };
                entryCount++;
            }

            if (i + 1 == tableBlockCount)
                break;

            StfsHashEntry hashEntry{};
            if (!readHashEntry(rootFile, baseOffset, tableBlockIndex, hashEntry))
            {
                sourceFiles.clear();
                return;
            }
            tableBlockIndex = readBigEndian<uint32_t>(&hashEntry.infoRaw) & 0xFFFFFF;
            if (tableBlockIndex == StfsEndOfChain)
            {
                break;
            }
        }
    }
    else if (volumeType == XContentVolumeType::SVOD)
    {
        sourceFiles.clear();

        // Close the root file and open all the files inside the directory with the same name instead.
        std::filesystem::path dataDirectory(contentPath.u8string() + u8".data");
        if (!std::filesystem::is_directory(dataDirectory))
        {
            return;
        }

        // Find all data files inside the directory.
        std::set<std::filesystem::path> orderedPaths;
        for (auto &entry : std::filesystem::directory_iterator(dataDirectory))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            orderedPaths.emplace(entry.path());
        }

        // Open each data file, falling back to streamed reads if mapping fails.
        for (auto &path : orderedPaths)
        {
            sourceFiles.emplace_back(path);
            if (!sourceFiles.back().isOpen())
            {
                sourceFiles.clear();
                return;
            }
        }

        if (sourceFiles.empty())
        {
            return;
        }

        // Determine the layout of the SVOD from the first file.
        const SourceFile::Reader firstFile(sourceFiles.front());
        auto matchesMagic = [&](size_t offset, const char* magic) {
            char bytes[20];
            const size_t length = strlen(magic);
            return length <= sizeof(bytes) && firstFile.read(offset, bytes, length)
                && std::memcmp(bytes, magic, length) == 0;
        };
        const char *RefMagic = "MICROSOFT*XBOX*MEDIA";
        size_t RefXSFMagicOffset = 0x12000;
        size_t SingleFileMagicOffset = 0xD000;
        if (metadata.svodDeviceDescriptor.features.bits.enhancedGdfLayout)
        {
            size_t EGDFMagicOffset = 0x2000;
            if (!matchesMagic(EGDFMagicOffset, RefMagic))
            {
                sourceFiles.clear();
                return;
            }

            svodBaseOffset = 0;
            svodMagicOffset = EGDFMagicOffset;
            svodLayoutType = SvodLayoutType::EnhancedGDF;
        }
        else if (matchesMagic(RefXSFMagicOffset, RefMagic))
        {
            const char *XSFMagic = "XSF";
            size_t XSFMagicOffset = 0x2000;
            svodBaseOffset = 0x10000;
            svodMagicOffset = 0x12000;

            if (matchesMagic(XSFMagicOffset, XSFMagic))
            {
                svodLayoutType = SvodLayoutType::XSF;
            }
            else
            {
                svodLayoutType = SvodLayoutType::Unknown;
            }
        }
        else if (matchesMagic(SingleFileMagicOffset, RefMagic))
        {
            svodBaseOffset = 0xB000;
            svodMagicOffset = 0xD000;
            svodLayoutType = SvodLayoutType::SingleFile;
        }
        else {
            sourceFiles.clear();
            return;
        }

        svodStartDataBlock = parseUint24(metadata.svodDeviceDescriptor.startDataBlockRaw);

        struct IterationStep
        {
            std::string fileNameBase;
            uint32_t blockIndex = 0;
            uint32_t ordinalIndex = 0;

            IterationStep() = default;
            IterationStep(std::string fileNameBase, uint32_t blockIndex, uint32_t ordinalIndex) : fileNameBase(fileNameBase), blockIndex(blockIndex), ordinalIndex(ordinalIndex) { }
        };

        std::stack<IterationStep> iterationStack;
        uint32_t rootBlock;
        if (!firstFile.read(svodMagicOffset + 0x14, &rootBlock, sizeof(rootBlock)))
        {
            sourceFiles.clear();
            return;
        }
        iterationStack.emplace("", rootBlock, 0);
        std::set<std::pair<size_t, size_t>> visitedEntries;

        IterationStep step;
        size_t fileOffset, fileIndex;
        char fileName[256];
        const uint8_t FileAttributeDirectory = 0x10;
        std::optional<SourceFile::Reader> reader;
        size_t readerIndex = SIZE_MAX;
        while (!iterationStack.empty())
        {
            step = iterationStack.top();
            iterationStack.pop();

            size_t ordinalOffset = step.ordinalIndex * 0x4;
            size_t blockOffset = ordinalOffset / 0x800;
            size_t trueOrdinalOffset = ordinalOffset % 0x800;
            blockToOffsetAndFile(svodLayoutType, svodStartDataBlock, svodBaseOffset, step.blockIndex + blockOffset, fileOffset, fileIndex);
            fileOffset += trueOrdinalOffset;
            if (fileIndex >= sourceFiles.size())
            {
                sourceFiles.clear();
                return;
            }

            if (readerIndex != fileIndex)
            {
                reader.emplace(sourceFiles[fileIndex]);
                readerIndex = fileIndex;
            }
            const SourceFile::Reader& file = *reader;
            SvodDirectoryEntry directoryEntry{};
            if (!file.read(fileOffset, &directoryEntry, sizeof(directoryEntry))
                || !visitedEntries.emplace(fileIndex, fileOffset).second)
            {
                sourceFiles.clear();
                return;
            }

            size_t nameOffset = fileOffset + sizeof(SvodDirectoryEntry);
            if (directoryEntry.nameLength == 0 || !file.read(nameOffset, fileName, directoryEntry.nameLength))
            {
                sourceFiles.clear();
                return;
            }

            fileName[directoryEntry.nameLength] = '\0';

            if (directoryEntry.nodeL)
            {
                iterationStack.emplace(step.fileNameBase, step.blockIndex, directoryEntry.nodeL);
            }

            if (directoryEntry.nodeR)
            {
                iterationStack.emplace(step.fileNameBase, step.blockIndex, directoryEntry.nodeR);
            }

            std::string fileNameUTF8 = step.fileNameBase + fileName;
            if (directoryEntry.attributes & FileAttributeDirectory)
            {
                if (directoryEntry.length > 0)
                {
                    iterationStack.emplace(fileNameUTF8 + "/", directoryEntry.dataBlock, 0);
                }
            }
            else
            {
                parsedFiles[fileNameUTF8] = { directoryEntry.length, directoryEntry.dataBlock, 0 };
            }
        }
    }
    else
    {
        sourceFiles.clear();
    }

    if (!sourceFiles.empty())
        fileMap = std::move(parsedFiles);
}

bool XContentFileSystem::load(const std::string &path, uint8_t *fileData, size_t fileDataMaxByteCount) const
{
    const auto it = fileMap.find(path);
    if (it == fileMap.end() || fileDataMaxByteCount < it->second.size || (fileData == nullptr && it->second.size != 0))
        return false;
    size_t offset = 0;
    return stream(path, [&](std::span<const uint8_t> bytes) {
        std::memcpy(fileData + offset, bytes.data(), bytes.size());
        offset += bytes.size();
        return true;
    });
}

bool XContentFileSystem::stream(const std::string& path, const ChunkSink& sink) const
{
    auto it = fileMap.find(path);
    if (it != fileMap.end())
    {
        if (sourceFiles.empty())
            return false;

        std::array<uint8_t, StfsBlockSize> buffer;

        if (volumeType == XContentVolumeType::STFS)
        {
            const SourceFile::Reader rootFile(sourceFiles.back());
            size_t remainingSize = it->second.size;
            uint32_t fileBlockIndex = it->second.blockIndex;
            for (uint32_t i = 0; i < it->second.blockCount && fileBlockIndex != StfsEndOfChain && remainingSize > 0; i++)
            {
                size_t blockSize = std::min(size_t(StfsBlockSize), remainingSize);
                size_t blockOffset = blockIndexToOffset(baseOffset, fileBlockIndex);
                if (!rootFile.read(blockOffset, buffer.data(), blockSize) || !sink(std::span(buffer).first(blockSize)))
                {
                    return false;
                }

                remainingSize -= blockSize;
                if (remainingSize > 0)
                {
                    StfsHashEntry hashEntry{};
                    if (!readHashEntry(rootFile, baseOffset, fileBlockIndex, hashEntry))
                        return false;

                    fileBlockIndex = readBigEndian<uint32_t>(&hashEntry.infoRaw) & 0xFFFFFF;
                }
            }

            return remainingSize == 0;
        }
        else if (volumeType == XContentVolumeType::SVOD)
        {
            std::optional<SourceFile::Reader> reader;
            size_t readerIndex = SIZE_MAX;
            size_t remainingSize = it->second.size;
            size_t currentBlock = it->second.blockIndex;
            while (remainingSize > 0)
            {
                size_t blockFileOffset, blockFileIndex;
                blockToOffsetAndFile(svodLayoutType, svodStartDataBlock, svodBaseOffset, currentBlock, blockFileOffset, blockFileIndex);
                if (blockFileIndex >= sourceFiles.size())
                {
                    return false;
                }

                if (readerIndex != blockFileIndex)
                {
                    reader.emplace(sourceFiles[blockFileIndex]);
                    readerIndex = blockFileIndex;
                }
                size_t blockSize = std::min(size_t(0x800), remainingSize);
                if (!reader->read(blockFileOffset, buffer.data(), blockSize) || !sink(std::span(buffer).first(blockSize)))
                {
                    return false;
                }

                remainingSize -= blockSize;
                currentBlock++;
            }

            return remainingSize == 0;
        }
        else
        {
            return false;
        }
    }
    else
    {
        return false;
    }
}

size_t XContentFileSystem::getSize(const std::string &path) const
{
    auto it = fileMap.find(path);
    if (it != fileMap.end())
    {
        return it->second.size;
    }
    else
    {
        return 0;
    }
}

bool XContentFileSystem::exists(const std::string &path) const
{
    return fileMap.find(path) != fileMap.end();
}

const std::string &XContentFileSystem::getName() const
{
    return name;
}

bool XContentFileSystem::empty() const
{
    return sourceFiles.empty();
}

std::unique_ptr<XContentFileSystem> XContentFileSystem::create(const std::filesystem::path &contentPath)
{
    std::unique_ptr<XContentFileSystem> xContentFS = std::make_unique<XContentFileSystem>(contentPath);
    if (!xContentFS->empty())
    {
        return xContentFS;
    }
    else
    {
        return nullptr;
    }
}

bool XContentFileSystem::check(const std::filesystem::path &contentPath)
{
    std::ifstream contentStream(contentPath, std::ios::binary);
    if (!contentStream.is_open())
    {
        return false;
    }

    uint32_t packageTypeUint = 0;
    contentStream.read((char *)(&packageTypeUint), sizeof(uint32_t));
    packageTypeUint = ByteSwap(packageTypeUint);
    XContentPackageType packageType = XContentPackageType(packageTypeUint);
    return packageType == XContentPackageType::CON || packageType == XContentPackageType::LIVE || packageType == XContentPackageType::PIRS;
}
