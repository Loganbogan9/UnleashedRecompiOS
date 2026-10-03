#include <os/app_lifecycle.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <mutex>
#include <thread>

#ifndef NDEBUG
#error This regression must run with Release assertions disabled.
#endif
#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::exit(1); } } while (false)

// The real audio lifecycle functions operate on a controlled SDL queue. Guest
// callbacks remain outside the device lock, as in the production producer.
using SDL_AudioDeviceID = uint32_t;
static bool outputPaused = false;
static unsigned pauseCalls = 0, clearCalls = 0;
static uint32_t queuedBytes = 0;
static void SDL_PauseAudioDevice(SDL_AudioDeviceID device, int paused)
{
    CHECK(device == 1);
    outputPaused = paused;
    ++pauseCalls;
}
static void SDL_ClearQueuedAudio(SDL_AudioDeviceID device)
{
    CHECK(device == 1);
    queuedBytes = 0;
    ++clearCalls;
}
static void SDL_QueueAudio(SDL_AudioDeviceID device, const void*, uint32_t size)
{
    CHECK(device == 1 && !outputPaused);
    queuedBytes += size;
}
#include "ios_audio_lifecycle.h"

static void CooperativePause()
{
    using namespace std::chrono_literals;
    auto& gate = app_lifecycle::GetExecutionGate();
    double delta = 1.0 / 60;
    app_lifecycle::WaitForFrame(delta);
    CHECK(delta == 1.0 / 60 && gate.IsActive());
    CHECK(gate.SetActive(false));
    CHECK(!gate.SetActive(false));
    // Frame/loading/audio workers all sleep, and resume must wake every waiter.
    std::array<std::future<double>, 6> workers;
    for (auto& worker : workers)
        worker = std::async(std::launch::async, [] {
            double frameDelta = 120.0;
            app_lifecycle::WaitForFrame(frameDelta);
            return frameDelta;
        });
    for (auto& worker : workers)
        CHECK(worker.wait_for(10ms) == std::future_status::timeout);
    CHECK(gate.SetActive(true));
    CHECK(!gate.SetActive(true));
    for (auto& worker : workers)
    {
        CHECK(worker.wait_for(1s) == std::future_status::ready);
        CHECK(worker.get() == 0.0);
    }
    // A pause during present can happen between two application updates. The
    // next update must still discard suspended time even though it never waits.
    delta = 180.0;
    app_lifecycle::WaitForFrame(delta);
    CHECK(delta == 0.0);
    delta = 1.0 / 60;
    app_lifecycle::WaitForFrame(delta);
    CHECK(delta == 1.0 / 60);
    for (unsigned cycle = 0; cycle < 100; ++cycle)
    {
        CHECK(gate.SetActive(false));
        auto audio = std::async(std::launch::async, [&] { return gate.WaitUntilActive(); });
        CHECK(audio.wait_for(1ms) == std::future_status::timeout);
        CHECK(gate.SetActive(true));
        CHECK(audio.wait_for(1s) == std::future_status::ready);
        CHECK(audio.get() == cycle + 2);
        delta = 30.0;
        app_lifecycle::WaitForFrame(delta);
        CHECK(delta == 0.0);
    }
}

static void AudioPause()
{
    XAudioSetAppActive(false); // Deactivation before audio is registered is safe.
    CHECK(pauseCalls == 0 && clearCalls == 0);
    g_audioDevice = 1;
    XAudioSetAppActive(true);
    CHECK(!outputPaused);
    const float samples[256]{};
    QueueAudioFrames(samples, sizeof(samples));
    CHECK(queuedBytes == sizeof(samples));
    XAudioSetAppActive(false);
    CHECK(outputPaused && queuedBytes == 0);
    // A callback that already started must not enqueue stale audio while paused.
    std::thread lateCallback([&] { QueueAudioFrames(samples, sizeof(samples)); });
    lateCallback.join();
    CHECK(queuedBytes == 0);
    XAudioSetAppActive(true);
    CHECK(!outputPaused && queuedBytes == 0);
    QueueAudioFrames(samples, sizeof(samples));
    CHECK(queuedBytes == sizeof(samples));
    // Serialize a stream of in-flight submissions with main-thread pauses.
    std::atomic<bool> stop = false;
    std::thread producer([&] {
        while (!stop)
            QueueAudioFrames(samples, sizeof(samples));
    });
    for (unsigned cycle = 0; cycle < 100; ++cycle)
    {
        XAudioSetAppActive(false);
        {
            std::lock_guard lock(g_audioDeviceMutex);
            CHECK(outputPaused && queuedBytes == 0);
        }
        XAudioSetAppActive(true);
    }
    stop = true;
    producer.join();
    XAudioSetAppActive(false);
    CHECK(queuedBytes == 0);
}

int main()
{
    CooperativePause();
    AudioPause();
    std::puts("iOS cooperative pause, resume timing and audio lifecycle passed with -O2 -DNDEBUG");
}
