#include <kernel/xex_load.h>
#include <cassert>
#include <iostream>
#include <vector>

template<class T>
void Write(std::vector<uint8_t>& bytes, size_t offset, const T& value)
{
    assert(offset + sizeof(value) <= bytes.size());
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

std::vector<uint8_t> MakeImage(bool basic = false)
{
    std::vector<uint8_t> bytes(0x240 + (basic ? 11 : 32));
    Xex2Header header{};
    header.magic = 0x58455832;
    header.headerSize = 0x240;
    header.securityOffset = 0x40;
    header.headerCount = 3;
    Write(bytes, 0, header);
    const uint32_t keys[] = { XEX_HEADER_ENTRY_POINT, XEX_HEADER_FILE_FORMAT_INFO, XEX_HEADER_RESOURCE_INFO };
    const uint32_t values[] = { 0x10000, 0x1d0, 0x210 };
    for (size_t i = 0; i < 3; ++i)
    {
        Xex2OptHeader optional{};
        optional.key = keys[i];
        optional.value = values[i];
        Write(bytes, sizeof(header) + i * sizeof(optional), optional);
    }
    Xex2SecurityInfo security{};
    security.imageSize = 32;
    security.loadAddress = 0x10000;
    Write(bytes, 0x40, security);
    Xex2OptFileFormatInfo format{};
    format.infoSize = basic ? 24 : 8;
    format.compressionType = basic ? XEX_COMPRESSION_BASIC : XEX_COMPRESSION_NONE;
    Write(bytes, 0x1d0, format);
    if (basic)
    {
        Xex2FileBasicCompressionBlock first{}, second{};
        first.dataSize = 3;
        first.zeroSize = 5;
        second.dataSize = 8;
        second.zeroSize = 16;
        Write(bytes, 0x1d8, first);
        Write(bytes, 0x1e0, second);
    }
    Xex2ResourceInfo resource{};
    resource.sizeOfHeader = sizeof(resource);
    resource.offset = 0x10008;
    resource.sizeOfData = 8;
    Write(bytes, 0x210, resource);
    return bytes;
}

bool Valid(std::span<const uint8_t> bytes)
{
    xex_load::Image image;
    std::string_view error;
    const bool valid = xex_load::Validate(bytes, 0x100000000ull, image, error);
    assert(valid == error.empty());
    if (!valid)
        assert(image.data.empty());
    return valid;
}

int main()
{
    for (bool basic : { false, true })
    {
        const auto good = MakeImage(basic);
        assert(Valid(good));
        for (size_t length = 0; length < good.size(); ++length)
            assert(!Valid(std::span(good).first(length)));
        std::vector<uint8_t> unaligned(good.size() + 1);
        std::copy(good.begin(), good.end(), unaligned.begin() + 1);
        assert(Valid(std::span(unaligned).subspan(1)));
    }

    const auto rejects = [](size_t offset, uint32_t value, bool basic = false)
    {
        auto bytes = MakeImage(basic);
        Write(bytes, offset, be<uint32_t>(value));
        assert(!Valid(bytes));
    };
    rejects(0, 0);                         // Bad signature.
    rejects(8, 0xfffffff0);                // Header extent.
    rejects(16, 0x200);                    // Truncated security information.
    rejects(20, 0xffffffff);               // Optional-header count overflow.
    rejects(28, 0x10001);                  // Unaligned entry point.
    rejects(28, 0x10020);                  // Entry outside loaded image.
    rejects(36, 0xfffffff0);               // File-format offset.
    rejects(44, 0xfffffff0);               // Resource offset.
    rejects(0x44, 0xffffffff);             // Image end exceeds guest space.
    rejects(0x150, 0xfffffff0);            // Load address plus image size wraps.
    rejects(0x1d0, 0xffffffff);            // File-format header length.
    rejects(0x1d4, 0x00010000);            // Encrypted input.
    rejects(0x1d4, 0x00000002);            // Unsupported compression.
    rejects(0x21c, 0xfffffffc);            // Resource address.
    rejects(0x220, 0xffffffff);            // Resource size.
    rejects(0x1d0, 23, true);              // Incomplete basic-compression block.
    rejects(0x1d8, 0xffffffff, true);      // Compressed source/target overflow.
    rejects(0x1dc, 0xffffffff, true);      // Zero-fill overflow.
    rejects(0x1e4, 15, true);              // Blocks underfill image.
    auto duplicate = MakeImage();
    Write(duplicate, 32, be<uint32_t>(XEX_HEADER_ENTRY_POINT));
    assert(!Valid(duplicate));

    // Exercise mutations under ASan/UBSan, including fields unrelated to loading.
    for (size_t offset = 0; offset < MakeImage().size(); ++offset)
    {
        auto bytes = MakeImage();
        bytes[offset] ^= 0xff;
        Valid(bytes);
    }
    std::cout << "XEX validation tests passed\n";
}
