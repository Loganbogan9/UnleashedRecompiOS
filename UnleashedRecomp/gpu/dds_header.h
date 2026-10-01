#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ddspp.h>

inline bool DecodeDdsHeader(const uint8_t* data, size_t dataSize, ddspp::Descriptor& descriptor)
{
    constexpr size_t baseHeaderSize = sizeof(ddspp::DDS_MAGIC) + sizeof(ddspp::Header);
    if (data == nullptr || dataSize < baseHeaderSize)
        return false;

    ddspp::Header header;
    std::memcpy(&header, data + sizeof(ddspp::DDS_MAGIC), sizeof(header));
    if (ddspp::is_dxt10(header) && dataSize < ddspp::MAX_HEADER_SIZE)
        return false;

    // ddspp reads a DX10 header even for a legacy DDS. A small legacy image
    // may end before that read, and source data need not be word-aligned.
    // Copy into a padded, aligned header to satisfy both requirements.
    alignas(ddspp::Header) unsigned char headerBytes[ddspp::MAX_HEADER_SIZE]{};
    const size_t copySize = dataSize < sizeof(headerBytes) ? dataSize : sizeof(headerBytes);
    std::memcpy(headerBytes, data, copySize);
    return ddspp::decode_header(headerBytes, descriptor) != ddspp::Error;
}
