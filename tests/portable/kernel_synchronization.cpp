#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <vector>
#include <xbox.h>

#ifndef NDEBUG
#error This regression must exercise optimized code with assertions disabled.
#endif

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::exit(1); } } while (false)

constexpr uint32_t STATUS_SUCCESS = 0;
constexpr uint32_t STATUS_TIMEOUT = 0x102;
constexpr uint32_t INFINITE = 0xffffffff;
constexpr bool TRUE = true;
struct KernelObject { virtual ~KernelObject() = default; virtual uint32_t Wait(uint32_t) = 0; };
struct Context { struct { uint32_t u32; } r13; };
thread_local Context* g_ppcContext;

// Fail every matching weak CAS without changing either value: a legal result
// of ARM's load-exclusive/store-exclusive sequence. Strong CAS forwards to the
// real atomic implementation, which retries these failures internally.
template<class T> struct TestAtomic : std::atomic<T>
{
    using std::atomic<T>::atomic;
    using std::atomic<T>::operator=;
    bool compare_exchange_weak(T& expected, T desired)
    {
        if (this->load() == expected)
            return false;
        return std::atomic<T>::compare_exchange_strong(expected, desired);
    }
    void wait(T old) const
    {
        // An auto-reset event must only sleep while it is not signaled.
        CHECK(!old);
        std::atomic<T>::wait(old);
    }
};

template<class T> struct TestAtomicRef : std::atomic_ref<T>
{
    explicit TestAtomicRef(T& value) : std::atomic_ref<T>(value) {}
    bool compare_exchange_weak(T& expected, T desired)
    {
        if (this->load() == expected)
            return false;
        return std::atomic_ref<T>::compare_exchange_strong(expected, desired);
    }
    void wait(T old) const
    {
        // Sleeping on a free lock leaves nobody responsible for waking us.
        CHECK(old != 0);
        std::atomic_ref<T>::wait(old);
    }
};

#include "kernel_waits.h"

template<class Predicate> void Until(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate())
    {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

void CriticalSection()
{
    Context context{{1}};
    g_ppcContext = &context;
    XRTL_CRITICAL_SECTION cs{};
    CHECK(RtlInitializeCriticalSection(&cs) == STATUS_SUCCESS);
    RtlEnterCriticalSection(&cs);
    CHECK(cs.OwningThread == 1 && cs.RecursionCount == 1);
    RtlEnterCriticalSection(&cs);
    CHECK(cs.OwningThread == 1 && cs.RecursionCount == 2);
    RtlLeaveCriticalSection(&cs);
    CHECK(cs.OwningThread == 1 && cs.RecursionCount == 1);
    RtlLeaveCriticalSection(&cs);
    CHECK(cs.OwningThread == 0 && cs.RecursionCount == 0);
}

void TryEnter()
{
    Context context{{1}};
    g_ppcContext = &context;
    XRTL_CRITICAL_SECTION cs{};
    RtlInitializeCriticalSectionAndSpinCount(&cs, 512);
    CHECK(cs.Header.Absolute == 2 && cs.LockCount == -1);
    CHECK(RtlTryEnterCriticalSection(&cs));
    CHECK(RtlTryEnterCriticalSection(&cs));
    CHECK(cs.OwningThread == 1 && cs.RecursionCount == 2);
    std::thread contender([&]
    {
        Context other{{2}};
        g_ppcContext = &other;
        CHECK(!RtlTryEnterCriticalSection(&cs));
    });
    contender.join();
    RtlLeaveCriticalSection(&cs);
    CHECK(cs.OwningThread == 1 && cs.RecursionCount == 1);
    RtlLeaveCriticalSection(&cs);
    CHECK(RtlTryEnterCriticalSection(&cs));
    RtlLeaveCriticalSection(&cs);
}

void AutoResetEvent()
{
    Event ready(false, true);
    CHECK(ready.Wait(INFINITE) == STATUS_SUCCESS);
    CHECK(ready.Wait(0) == STATUS_TIMEOUT);
    ready.Set();
    CHECK(ready.Wait(0) == STATUS_SUCCESS);
    CHECK(ready.Wait(0) == STATUS_TIMEOUT);
    ready.Set();
    ready.Reset();
    CHECK(ready.Wait(0) == STATUS_TIMEOUT);
    XKEVENT header{};
    header.Type = 1;
    header.SignalState = 1;
    Event fromGuest(&header);
    CHECK(fromGuest.Wait(INFINITE) == STATUS_SUCCESS);
    CHECK(fromGuest.Wait(0) == STATUS_TIMEOUT);
}

void Contention()
{
    XRTL_CRITICAL_SECTION cs{};
    RtlInitializeCriticalSection(&cs);
    constexpr unsigned workers = 8;
    constexpr unsigned iterations = 2000;
    unsigned count = 0;
    std::atomic<bool> begin{false};
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < workers; ++i)
        threads.emplace_back([&, i]
        {
            Context context{{i + 1}};
            g_ppcContext = &context;
            begin.wait(false);
            for (unsigned j = 0; j < iterations; ++j)
            {
                RtlEnterCriticalSection(&cs);
                RtlEnterCriticalSection(&cs);
                ++count;
                RtlLeaveCriticalSection(&cs);
                RtlLeaveCriticalSection(&cs);
            }
        });
    begin = true;
    begin.notify_all();
    for (auto& thread : threads)
        thread.join();
    CHECK(count == workers * iterations);
    CHECK(cs.OwningThread == 0 && cs.RecursionCount == 0);

    for (bool manual : {false, true})
    {
        Event event(manual, false);
        std::atomic<unsigned> started{0}, completed{0};
        threads.clear();
        for (unsigned i = 0; i < workers; ++i)
            threads.emplace_back([&]
            {
                ++started;
                CHECK(event.Wait(INFINITE) == STATUS_SUCCESS);
                ++completed;
            });
        Until([&] { return started.load() == workers; });
        if (manual)
        {
            event.Set();
            Until([&] { return completed.load() == workers; });
            CHECK(event.Wait(0) == STATUS_SUCCESS);
            event.Reset();
        }
        else
        {
            for (unsigned i = 1; i <= workers; ++i)
            {
                event.Set();
                Until([&] { return completed.load() == i; });
                CHECK(event.Wait(0) == STATUS_TIMEOUT);
            }
        }
        for (auto& thread : threads)
            thread.join();
        CHECK(event.Wait(0) == STATUS_TIMEOUT);
    }
}

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    const std::string_view test = argv[1];
    if (test == "critical-section") CriticalSection();
    else if (test == "try-enter") TryEnter();
    else if (test == "auto-reset-event") AutoResetEvent();
    else if (test == "contention") Contention();
    else CHECK(false);
    std::printf("%s passed with -O2 -DNDEBUG\n", argv[1]);
}
