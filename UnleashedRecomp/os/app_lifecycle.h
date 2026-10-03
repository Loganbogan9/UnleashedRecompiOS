#pragma once

#include <atomic>
#include <cstdint>

namespace app_lifecycle
{
    // Cooperatively stop work at frame/task boundaries, after callers have
    // released locks. UIKit never waits for these workers to acknowledge a pause.
    class ExecutionGate
    {
        // Odd values are paused; each resume advances the generation. Keeping
        // both in one atomic also detects a pause/resume between frame updates.
        std::atomic<uint64_t> state{0};

    public:
        bool IsActive() const { return (state.load() & 1) == 0; }

        bool SetActive(bool active)
        {
            auto previous = state.load();
            do
            {
                if (((previous & 1) == 0) == active)
                    return false;
            } while (!state.compare_exchange_strong(previous, previous + 1));

            if (active)
                state.notify_all();
            return true;
        }

        uint64_t WaitUntilActive() const
        {
            auto current = state.load();
            while (current & 1)
            {
                state.wait(current);
                current = state.load();
            }
            return current >> 1;
        }
    };

    inline ExecutionGate& GetExecutionGate()
    {
        static ExecutionGate gate;
        return gate;
    }

    inline void WaitForFrame(double& deltaTime)
    {
        const auto resumeGeneration = GetExecutionGate().WaitUntilActive();
        static thread_local uint64_t previousResumeGeneration = 0;
        if (resumeGeneration != previousResumeGeneration)
        {
            // Guest clocks keep advancing while suspended. Discard the gap
            // on each frame-producing thread, including the loading thread.
            deltaTime = 0.0;
            previousResumeGeneration = resumeGeneration;
        }
    }
}
