#pragma once

#ifndef _WIN32
#define MEM_COMMIT  0x00001000  
#define MEM_RESERVE 0x00002000  
#endif

struct Memory
{
    uint8_t* base{};
    size_t guardPageSize{};
    // Initialization runs before logging is available. Retain native failures
    // so startup can also report them in the persistent application log.
    const char* initializationFailureStage{ "none" };
    uint32_t initializationNativeError{};

    Memory();

    bool IsInMemoryRange(const void* host) const noexcept
    {
        const auto address = reinterpret_cast<uintptr_t>(host);
        const auto start = reinterpret_cast<uintptr_t>(base);
        return base != nullptr && address >= start && address - start < PPC_MEMORY_SIZE;
    }

    void* Translate(size_t offset) const noexcept
    {
        if (offset)
            assert(offset < PPC_MEMORY_SIZE);

        return base + offset;
    }

    uint32_t MapVirtual(const void* host) const noexcept
    {
        if (host == nullptr)
            return 0;

        assert(IsInMemoryRange(host));

        return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(host) - reinterpret_cast<uintptr_t>(base));
    }

    PPCFunc* FindFunction(uint32_t guest) const noexcept
    {
        return PPC_LOOKUP_FUNC(base, guest);
    }

    void InsertFunction(uint32_t guest, PPCFunc* host)
    {
        PPC_LOOKUP_FUNC(base, guest) = host;
    }
};

extern "C" void* MmGetHostAddress(uint32_t ptr);
extern Memory g_memory;
