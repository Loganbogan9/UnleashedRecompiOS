#include <stdafx.h>
#include <kernel/semaphore.h>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <thread>

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::abort(); } } while (false)

int main()
{
    Semaphore semaphore(0, 16);
    CHECK(semaphore.Wait(0) == STATUS_TIMEOUT);
    CHECK(semaphore.count.load() == 0);
    uint32_t previous = 99;
    semaphore.Release(3, &previous);
    CHECK(previous == 0);
    CHECK(semaphore.Wait(0) == STATUS_SUCCESS);
    CHECK(semaphore.Wait(0) == STATUS_SUCCESS);
    CHECK(semaphore.Wait(0) == STATUS_SUCCESS);
    CHECK(semaphore.Wait(0) == STATUS_TIMEOUT);
    CHECK(semaphore.count.load() == 0);

    XKSEMAPHORE guest{};
    guest.Header.SignalState = 2;
    guest.Limit = 16;
    Semaphore fromGuest(&guest);
    CHECK(fromGuest.Wait(0) == STATUS_SUCCESS);
    CHECK(fromGuest.count.load() == 1);

    constexpr unsigned workers = 16;
    constexpr unsigned rounds = 256;
    std::barrier begin(workers + 1);
    std::barrier end(workers + 1);
    std::atomic<unsigned> acquired;
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < workers; ++i)
    {
        threads.emplace_back([&]
        {
            for (unsigned round = 0; round < rounds; ++round)
            {
                begin.arrive_and_wait();
                if (semaphore.Wait(0) == STATUS_SUCCESS)
                    acquired.fetch_add(1);
                end.arrive_and_wait();
            }
        });
    }
    for (unsigned round = 0; round < rounds; ++round)
    {
        acquired = 0;
        semaphore.count = workers;
        begin.arrive_and_wait();
        end.arrive_and_wait();
        // Every contender has a permit; a failed CAS must not report timeout
        // just because another contender consumed a different permit first.
        CHECK(acquired.load() == workers);
        CHECK(semaphore.count.load() == 0);
    }
    for (auto& thread : threads)
        thread.join();

    std::atomic<bool> completed = false;
    std::thread waiter([&]
    {
        CHECK(semaphore.Wait(INFINITE) == STATUS_SUCCESS);
        completed = true;
    });
    semaphore.Release(1, nullptr);
    waiter.join();
    CHECK(completed.load());
    CHECK(semaphore.count.load() == 0);
    std::puts("Production semaphore polling and contention regressions passed");
}
