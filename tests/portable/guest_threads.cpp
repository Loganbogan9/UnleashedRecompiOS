#include <stdafx.h>
#include <cpu/guest_thread.h>
#include <cpu/ppc_context.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>

Memory g_memory;
Heap g_userHeap;
static bool failThreadCreation = false;
static bool failStackSize = false;
static std::atomic<unsigned> nativeJoins;
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*function)(void*), void* arg)
{
    return failThreadCreation ? EAGAIN : __real_pthread_create(thread, attr, function, arg);
}
extern "C" int __real_pthread_attr_setstacksize(pthread_attr_t*, size_t);
extern "C" int __wrap_pthread_attr_setstacksize(pthread_attr_t* attr, size_t size)
{
    return failStackSize ? EINVAL : __real_pthread_attr_setstacksize(attr, size);
}
extern "C" int __real_pthread_join(pthread_t, void**);
extern "C" int __wrap_pthread_join(pthread_t thread, void** result)
{
    nativeJoins.fetch_add(1);
    return __real_pthread_join(thread, result);
}
#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::abort(); } } while (false)
static std::atomic<uint32_t> executed;
static void GuestFunction(PPCContext& context, uint8_t*)
{
    CHECK(GetPPCContext() == &context);
    CHECK(GuestThread::GetCurrentThreadId() != 0);
    executed.fetch_add(1);
    context.r3.u32 += 1;
}
int main()
{
    CHECK(g_memory.base != nullptr);
    g_userHeap.Init();
    g_memory.InsertFunction(0x100, GuestFunction);
    CHECK(GuestThread::Start({ 0x100, 41, 0 }) == 42);
    CHECK(GetPPCContext() == nullptr);
    CHECK(GuestThread::Start({ 0x100, 42, 0 }) == 43);
    CHECK(GetPPCContext() == nullptr); // context can be reused after a guest call returns

    const auto baselineAllocated = o1heapGetDiagnostics(g_userHeap.physicalHeap).allocated;
    uint32_t id = 0;
    failThreadCreation = true;
    CHECK(GuestThread::Start({ 0x100, 0, 0 }, &id) == nullptr);
    failThreadCreation = false;
    CHECK(nativeJoins.load() == 0);
    CHECK(o1heapGetDiagnostics(g_userHeap.physicalHeap).allocated == baselineAllocated);
    failStackSize = true;
    CHECK(GuestThread::Start({ 0x100, 0, 0 }, &id) == nullptr);
    failStackSize = false;
    CHECK(nativeJoins.load() == 0);
    CHECK(o1heapGetDiagnostics(g_userHeap.physicalHeap).allocated == baselineAllocated);

    auto* thread = GuestThread::Start({ 0x100, 0, 0 }, &id);
    CHECK(thread != nullptr && id != 0);
    CHECK(thread->Wait(INFINITE) == STATUS_WAIT_0);
    CHECK(thread->Wait(INFINITE) == STATUS_WAIT_0);
    CHECK(thread->GetThreadId() == id); // guest identity survives completion
    CHECK(nativeJoins.load() == 1);
    DestroyKernelObject(thread);
    CHECK(nativeJoins.load() == 1); // destruction does not join again

    thread = GuestThread::Start({ 0x100, 0, 1 }, &id);
    CHECK(thread != nullptr);
    CHECK(thread->suspended.load());
    thread->suspended = false;
    thread->suspended.notify_all();
    std::thread waitA([&] { CHECK(thread->Wait(INFINITE) == STATUS_WAIT_0); });
    std::thread waitB([&] { CHECK(thread->Wait(INFINITE) == STATUS_WAIT_0); });
    waitA.join();
    waitB.join();
    CHECK(thread->joined);
    DestroyKernelObject(thread);
    CHECK(executed.load() == 4);
    CHECK(o1heapGetDiagnostics(g_userHeap.physicalHeap).allocated == baselineAllocated);
    CHECK(o1heapDoInvariantsHold(g_userHeap.physicalHeap));
    CHECK(o1heapDoInvariantsHold(g_userHeap.heap));
    munmap(g_memory.base, PPC_MEMORY_SIZE);
    std::puts("Production pthread creation, guest context, and repeated wait regressions passed");
}
