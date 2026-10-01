#include <stdafx.h>
#include "memory.h"
#include <cerrno>
#include <cstdio>

Memory::Memory()
{
#ifdef _WIN32
    base = (uint8_t*)VirtualAlloc((void*)0x100000000ull, PPC_MEMORY_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (base == nullptr)
        base = (uint8_t*)VirtualAlloc(nullptr, PPC_MEMORY_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (base == nullptr)
        return;

    SYSTEM_INFO systemInfo;
    GetSystemInfo(&systemInfo);
    guardPageSize = systemInfo.dwPageSize;
    DWORD oldProtect;
    if (!VirtualProtect(base, systemInfo.dwPageSize, PAGE_NOACCESS, &oldProtect))
    {
        std::fprintf(stderr, "Guest memory guard page failed (error %lu).\n", GetLastError());
        VirtualFree(base, 0, MEM_RELEASE);
        base = nullptr;
        guardPageSize = 0;
        return;
    }
#else
    base = (uint8_t*)mmap((void*)0x100000000ull, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

    if (base == (uint8_t*)MAP_FAILED)
        base = (uint8_t*)mmap(NULL, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

    if (base == (uint8_t*)MAP_FAILED)
    {
        std::fprintf(stderr, "Failed to reserve %llu bytes of guest memory (errno %d).\n", static_cast<unsigned long long>(PPC_MEMORY_SIZE), errno);
        base = nullptr;
        return;
    }

    // Use the host page size explicitly (Apple devices use 16 KiB pages).
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0 || static_cast<size_t>(pageSize) > PPC_MEMORY_SIZE
        || mprotect(base, static_cast<size_t>(pageSize), PROT_NONE) != 0)
    {
        std::fprintf(stderr, "Guest memory guard page failed (page size %ld, errno %d).\n", pageSize, errno);
        munmap(base, PPC_MEMORY_SIZE);
        base = nullptr;
        return;
    }
    guardPageSize = static_cast<size_t>(pageSize);
#endif

    for (size_t i = 0; PPCFuncMappings[i].guest != 0; i++)
    {
        if (PPCFuncMappings[i].host != nullptr)
            InsertFunction(PPCFuncMappings[i].guest, PPCFuncMappings[i].host);
    }
}

void* MmGetHostAddress(uint32_t ptr)
{
    return g_memory.Translate(ptr);
}
