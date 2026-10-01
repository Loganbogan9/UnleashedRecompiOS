#include <stdafx.h>
#include <kernel/memory.h>
#include <kernel/heap.h>
#include <kernel/function.h>
#include <csignal>
#include <cstdio>
#include <sys/resource.h>
#include <sys/wait.h>

Memory g_memory;
Heap g_userHeap;
uint32_t RtlAllocateHeap(uint32_t, uint32_t, uint32_t);
uint32_t RtlReAllocateHeap(uint32_t, uint32_t, uint32_t, uint32_t);
uint32_t XAllocMem(uint32_t, uint32_t);

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::abort(); } } while (false)

static void ReturnNull(PPCContext& ctx, uint8_t*) { ctx.r3.u32 = 0; }
static void EchoPointer(PPCContext&, uint8_t*) { }

int main()
{
    CHECK(g_memory.base != nullptr);
    CHECK(g_memory.MapVirtual(nullptr) == 0);
    CHECK(g_memory.MapVirtual(g_memory.Translate(0x20000)) == 0x20000);
    CHECK(!g_memory.IsInMemoryRange(nullptr));
    CHECK(!g_memory.IsInMemoryRange(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(g_memory.base) - 1)));
    CHECK(!g_memory.IsInMemoryRange(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(g_memory.base) + PPC_MEMORY_SIZE)));
    CHECK(g_memory.IsInMemoryRange(g_memory.Translate(0xFFFFFFFF)));

    PPCContext context{};
    context.r3.u64 = 0xDEADBEEF;
    ArgTranslator::SetValue<void*>(context, g_memory.base, 0, nullptr);
    CHECK(context.r3.u64 == 0);
    SetPPCContext(context);
    CHECK(GuestToHostFunction<void*>(ReturnNull) == nullptr);
    CHECK(GuestToHostFunction<void*>(EchoPointer, static_cast<void*>(nullptr)) == nullptr);
    void* translated = g_memory.Translate(0x20000);
    CHECK(GuestToHostFunction<void*>(EchoPointer, translated) == translated);
    g_ppcContext = nullptr;
    const long pageSize = sysconf(_SC_PAGESIZE);
    CHECK(pageSize > 0);
    CHECK(g_memory.guardPageSize == static_cast<size_t>(pageSize));
    g_memory.base[pageSize] = 1;

    g_userHeap.Init();
    CHECK(g_userHeap.heap != nullptr && g_userHeap.physicalHeap != nullptr);
    for (size_t alignment : { 0, 16, 32, 256, 4096, 16384 })
    {
        for (size_t size : { 0, 1, 16, 31, 1024, 65537 })
        {
            void* ptr = g_userHeap.AllocPhysical(size, alignment);
            CHECK(ptr != nullptr);
            CHECK(reinterpret_cast<uintptr_t>(ptr) % (alignment == 0 ? 4096 : alignment) == 0);
            CHECK(g_userHeap.Size(ptr) == std::max<size_t>(size, 1));
            std::memset(ptr, 0x5A, std::max<size_t>(size, 1));
            g_userHeap.Free(ptr);
        }
    }
    CHECK(g_userHeap.AllocPhysical(1, 17) == nullptr);
    CHECK(g_userHeap.AllocPhysical(std::numeric_limits<size_t>::max(), 4096) == nullptr);
    CHECK(g_userHeap.AllocPhysical(0xFFFFFFFF, 4096) == nullptr);
    g_userHeap.Free(nullptr);
    CHECK(RtlAllocateHeap(0, 8, 0xFFFFFFFF) == 0);
    CHECK(XAllocMem(0xFFFFFFFF, 0xC0000000) == 0);
    CHECK(XAllocMem(0xFFFFFFFF, 0x40000000) == 0);
    const uint32_t oldAddress = RtlAllocateHeap(0, 8, 64);
    CHECK(oldAddress != 0);
    auto* oldData = static_cast<uint8_t*>(g_memory.Translate(oldAddress));
    CHECK(std::all_of(oldData, oldData + 64, [](uint8_t v) { return v == 0; }));
    std::memset(oldData, 0xAB, 64);
    CHECK(RtlReAllocateHeap(0, 8, oldAddress, 0xFFFFFFFF) == 0);
    CHECK(std::all_of(oldData, oldData + 64, [](uint8_t v) { return v == 0xAB; }));
    const uint32_t newAddress = RtlReAllocateHeap(0, 8, oldAddress, 128);
    CHECK(newAddress != 0);
    auto* newData = static_cast<uint8_t*>(g_memory.Translate(newAddress));
    CHECK(std::all_of(newData, newData + 64, [](uint8_t v) { return v == 0xAB; }));
    CHECK(std::all_of(newData + 64, newData + 128, [](uint8_t v) { return v == 0; }));
    g_userHeap.Free(newData);
    CHECK(o1heapDoInvariantsHold(g_userHeap.heap));
    CHECK(o1heapDoInvariantsHold(g_userHeap.physicalHeap));

    // Both mmap attempts must report failure cleanly. Restrict only a child,
    // since ASan reserves a large address range of its own in the parent.
    const pid_t child = fork();
    CHECK(child != -1);
    if (child == 0)
    {
        struct rlimit limit { 256 * 1024 * 1024, 256 * 1024 * 1024 };
        if (setrlimit(RLIMIT_AS, &limit) != 0)
            std::_Exit(2);
        Memory failure;
        std::_Exit(failure.base == nullptr ? 0 : 3);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    munmap(g_memory.base, PPC_MEMORY_SIZE);
    std::puts("Production guest memory and O1Heap allocation regressions passed");
}
