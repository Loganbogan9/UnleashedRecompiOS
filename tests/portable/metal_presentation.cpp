#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <plume_metal_diagnostics.h>
#include <plume_metal_lifecycle.h>

#ifndef NDEBUG
#error This regression must run with Release assertions disabled.
#endif
#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::exit(1); } } while (false)

namespace CA { struct MetalDrawable; }
namespace MTL { struct CommandBuffer; }
struct MetalAutoreleasePool;
static thread_local MetalAutoreleasePool* currentPool;
struct MetalAutoreleasePool
{
    MetalAutoreleasePool* parent = currentPool;
    std::vector<CA::MetalDrawable*> objects;
    std::vector<MTL::CommandBuffer*> buffers;
    MetalAutoreleasePool() { currentPool = this; }
    ~MetalAutoreleasePool();
};

namespace NS
{
template<typename T> using SharedPtr = std::shared_ptr<T>;
template<typename T> SharedPtr<T> RetainPtr(T* object)
{
    object->retain();
    return SharedPtr<T>(object, [](T* retained) { retained->release(); });
}
struct String { std::string text; const char* utf8String() const { return text.c_str(); } };
struct Error
{
    String description{"GPU access is not permitted"};
    long code() const { return 7; }
    const String* localizedDescription() const { return &description; }
};
}
#define MTLSTR(text) (text)

namespace MTL
{
struct Device {};
struct Event { uint64_t value = 0; };
struct Texture
{
    uint32_t pixelWidth = 640, pixelHeight = 360;
    uint32_t width() const { return pixelWidth; }
    uint32_t height() const { return pixelHeight; }
    int pixelFormat() const { return 1; }
};
struct Drawable
{
    std::vector<std::function<void(Drawable*)>> displayed;
    void addPresentedHandler(std::function<void(Drawable*)> callback) { displayed.push_back(std::move(callback)); }
    void present() { auto handlers = std::move(displayed); for (auto& handler : handlers) handler(this); }
    double presentedTime() const { return 1.0; }
};
enum CommandBufferStatus { CommandBufferStatusCompleted, CommandBufferStatusError };
struct CommandQueue;
struct CommandBuffer
{
    CommandQueue* queue = nullptr;
    std::atomic<int> references{1};
    std::atomic<unsigned> schedulingWaits{0};
    CommandBuffer* retain() { ++references; return this; }
    CommandQueue* commandQueue() const { return queue; }
    NS::String name;
    NS::Error failure;
    CommandBufferStatus result = CommandBufferStatusCompleted;
    std::vector<std::function<void(CommandBuffer*)>> scheduled, completed;
    std::atomic<bool> enqueued = false, committed = false, scheduledOnce = false;
    bool finished = false;
    void setLabel(const char* label) { name.text = label; }
    const NS::String* label() const { return &name; }
    const NS::Error* error() const { return result == CommandBufferStatusError ? &failure : nullptr; }
    CommandBufferStatus status() const { return result; }
    void enqueue() { enqueued = true; }
    void encodeWait(Event*, uint64_t) {}
    void encodeSignalEvent(Event*, uint64_t) {}
    void addScheduledHandler(std::function<void(CommandBuffer*)> handler) { scheduled.push_back(std::move(handler)); }
    void addCompletedHandler(std::function<void(CommandBuffer*)> handler) { completed.push_back(std::move(handler)); }
    void commit() { CHECK(enqueued); retain(); committed = true; }
    void release() { CHECK(references.fetch_sub(1) > 0); }
    void waitUntilScheduled()
    {
        CHECK(committed);
        ++schedulingWaits;
        Schedule();
    }
    void Schedule()
    {
        CHECK(committed);
        if (!scheduledOnce.exchange(true))
            for (auto& handler : scheduled) handler(this);
    }
    void Finish(bool fail = false)
    {
        CHECK(committed && !finished);
        result = fail ? CommandBufferStatusError : CommandBufferStatusCompleted;
        if (!fail) Schedule();
        for (auto& handler : completed) handler(this);
        scheduled.clear();
        completed.clear();
        finished = true;
        release();
    }
};
struct CommandQueue
{
    std::vector<std::unique_ptr<CommandBuffer>> buffers;
    bool failCreation = false;
    CommandBuffer* commandBufferWithUnretainedReferences()
    {
        if (failCreation) return nullptr;
        buffers.push_back(std::make_unique<CommandBuffer>());
        auto* buffer = buffers.back().get();
        buffer->queue = this;
        if (currentPool) currentPool->buffers.push_back(buffer);
        return buffer;
    }
    void FinishAll()
    {
        for (auto& buffer : buffers) if (buffer->committed && !buffer->finished) buffer->Finish();
    }
};
}

struct Size { double width, height; };
static Size CGSizeMake(double width, double height) { return {width, height}; }
namespace CA
{
struct MetalDrawable : MTL::Drawable
{
    std::atomic<int> references{0};
    MTL::Texture image;
    MetalDrawable* retain() { ++references; return this; }
    void release() { CHECK(references.fetch_sub(1) > 0); }
    MTL::Texture* texture() { return &image; }
};
struct MetalLayer
{
    std::array<MetalDrawable, 3> pool;
    unsigned unavailable = 0;
    void setDevice(MTL::Device*) {}
    void setPixelFormat(int) {}
    void setDrawableSize(Size) {}
    MetalDrawable* nextDrawable()
    {
        if (unavailable) { --unavailable; return nullptr; }
        for (auto& drawable : pool)
            if (drawable.references == 0)
            {
                CHECK(currentPool);
                drawable.retain();
                currentPool->objects.push_back(&drawable);
                return &drawable;
            }
        return nullptr;
    }
    void CheckReleased() const { for (auto& drawable : pool) CHECK(drawable.references == 0); }
};
}
MetalAutoreleasePool::~MetalAutoreleasePool()
{
    for (auto* drawable : objects) drawable->release();
    for (auto* buffer : buffers) buffer->release();
    currentPool = parent;
}

namespace plume
{
constexpr uint32_t MAX_DRAWABLES = 3;
enum class RenderFormat { UNKNOWN, COLOR };
enum class RenderTextureFlag { RENDER_TARGET };
struct RenderTexture {};
struct RenderTextureDesc { uint32_t width = 0, height = 0; RenderFormat format{}; RenderTextureFlag flags{}; };
struct RenderWindow { void* window = nullptr; void* view = nullptr; };
struct RenderCommandSemaphore {};
struct MetalCommandSemaphore : RenderCommandSemaphore { MTL::Event* mtl; uint64_t mtlEventValue = 1; };
struct MetalDevice { MTL::Device* mtl = nullptr; };
struct MetalCommandQueue { MetalDevice* device; MTL::CommandQueue* mtl; };
struct MetalCommandList { MTL::CommandBuffer* mtl; void commit(); };
struct MetalDrawable { CA::MetalDrawable* mtl = nullptr; RenderTextureDesc desc; ~MetalDrawable(); };
struct CocoaWindowAttributes { int x = 0, y = 0, width = 640, height = 360; };
struct CocoaWindow { explicit CocoaWindow(void*) {} void getWindowAttributes(CocoaWindowAttributes* a) const { *a = {}; } };
static int mapPixelFormat(RenderFormat) { return 1; }
static RenderFormat mapRenderFormat(int) { return RenderFormat::COLOR; }
}
#include "metal_swapchain.h"

struct Fixture
{
    CA::MetalLayer layer;
    MTL::CommandQueue queue;
    plume::MetalDevice device;
    plume::MetalCommandQueue wrapper{&device, &queue};
    MTL::Event event;
    plume::MetalCommandSemaphore semaphore{{}, &event, 1};
    std::unique_ptr<plume::MetalSwapChain> chain = std::make_unique<plume::MetalSwapChain>(
        &wrapper, plume::RenderWindow{nullptr, &layer}, 3, plume::RenderFormat::COLOR, 1);
    ~Fixture()
    {
        plume::setMetalAppActive(false);
        queue.FinishAll();
        chain.reset();
        layer.CheckReleased();
        for (auto& buffer : queue.buffers) CHECK(buffer->references == 0);
        plume::setMetalAppActive(true);
    }
    uint32_t Acquire()
    {
        uint32_t index = 999;
        CHECK(chain->acquireTexture(&semaphore, &index));
        CHECK(index < 3 && chain->drawables[index].mtl);
        return index;
    }
    void Present(uint32_t index) { CHECK(chain->present(index, nullptr, 0)); }
};

static void Ownership()
{
    Fixture f;
    for (unsigned i = 0; i < 3; ++i)
    {
        const auto index = f.Acquire();
        CHECK(index == i);
        f.Present(index);
        CHECK(f.chain->drawables[index].mtl == nullptr);
    }
    const auto cursor = f.chain->currentAvailableDrawableIndex;
    std::thread completion([&] { f.queue.FinishAll(); });
    completion.join();
    CHECK(f.chain->currentAvailableDrawableIndex == cursor);
    f.layer.CheckReleased();
    // Keep rendering beyond the pool size, under delayed and immediate callbacks.
    for (unsigned i = 0; i < 40; ++i)
    {
        const auto index = f.Acquire();
        CHECK(index == i % 3);
        f.Present(index);
        f.queue.FinishAll();
        f.chain->wait();
        f.layer.CheckReleased();
    }
    // Layer layout can change independently of the cached window geometry.
    f.layer.pool[0].image.pixelWidth = 641;
    const auto index = f.Acquire();
    CHECK(f.chain->drawables[index].desc.width == 641);
    f.Present(index);
}

static void Unavailable()
{
    Fixture f;
    f.layer.unavailable = 4;
    uint32_t output = 999;
    for (unsigned i = 0; i < 4; ++i)
    {
        CHECK(!f.chain->acquireTexture(&f.semaphore, &output));
        CHECK(output == 999 && f.chain->currentAvailableDrawableIndex == 0);
        CHECK(f.queue.buffers.empty() && f.semaphore.mtlEventValue == 1);
    }
    f.queue.failCreation = true;
    CHECK(!f.chain->acquireTexture(&f.semaphore, &output));
    f.layer.CheckReleased();
    CHECK(output == 999 && f.chain->currentAvailableDrawableIndex == 0);
    f.queue.failCreation = false;
    CHECK(f.Acquire() == 0);
    f.queue.failCreation = true;
    CHECK(!f.chain->present(0, nullptr, 0));
    f.layer.CheckReleased();
    f.queue.failCreation = false;
    f.Present(f.Acquire());
    f.queue.FinishAll();
    CHECK(!f.chain->present(999, nullptr, 0));
    CHECK(!f.chain->acquireTexture(nullptr, &output));
    CHECK(!f.chain->acquireTexture(&f.semaphore, nullptr));
}

static void Callbacks()
{
    Fixture f;
    f.Present(f.Acquire());
    auto state = f.chain->presentState;
    std::thread completion([&] { f.queue.FinishAll(); });
    f.chain->wait();
    completion.join();
    CHECK(state->lastCompletedId == 1);
    f.Present(f.Acquire());
    std::weak_ptr<plume::MetalSwapChain::PresentState> weakState = state;
    state.reset();
    f.chain.reset();
    CHECK(!weakState.expired());
    std::thread lateCompletion([&] { f.queue.FinishAll(); });
    lateCompletion.join();
    CHECK(weakState.expired());
    f.layer.CheckReleased();
}

static unsigned displayed = 0, errors = 0;
static std::string lastError;
static void Diagnostic(const char* message, bool error)
{
    if (error) { ++errors; lastError = message; }
    else { ++displayed; CHECK(std::string_view(message).starts_with("Metal drawable displayed:")); }
}
static void Diagnostics()
{
    plume::setMetalDiagnosticCallback(Diagnostic);
    Fixture f;
    for (unsigned i = 0; i < 4; ++i) { f.Present(f.Acquire()); f.queue.FinishAll(); }
    CHECK(displayed == 3 && errors == 0);
    f.Present(f.Acquire());
    // A failed present never invokes the scheduled/presented callbacks, but
    // must release ownership and wake the CPU latency waiter on completion.
    f.queue.buffers.back()->Finish(true);
    f.queue.FinishAll();
    f.chain->wait();
    CHECK(errors == 1 && displayed == 3);
    CHECK(lastError.find("Present Command Buffer") != std::string::npos);
    CHECK(lastError.find("code=7") != std::string::npos);
    f.layer.CheckReleased();
    auto* render = f.queue.commandBufferWithUnretainedReferences();
    render->setLabel("Render Command Buffer");
    plume::MetalCommandList list{render};
    list.commit();
    CHECK(list.mtl == nullptr);
    render->Finish(true);
    CHECK(errors == 2 && lastError.find("Render Command Buffer") != std::string::npos);
    plume::setMetalDiagnosticCallback(nullptr);
}

static void Lifecycle()
{
    using namespace std::chrono_literals;
    Fixture f;
    MTL::CommandQueue copyQueue;
    auto* draw0 = f.queue.commandBufferWithUnretainedReferences();
    plume::MetalCommandList first{draw0};
    first.commit();
    auto* copy = copyQueue.commandBufferWithUnretainedReferences();
    plume::MetalCommandList upload{copy};
    upload.commit();
    auto* draw1 = f.queue.commandBufferWithUnretainedReferences();
    plume::MetalCommandList second{draw1};
    second.commit();
    // Pause must schedule the last committed buffer on EACH queue without
    // waiting for GPU completion, and release the gate's retained ownership.
    plume::setMetalAppActive(false);
    CHECK(draw0->schedulingWaits == 0 && draw1->schedulingWaits == 1);
    CHECK(copy->schedulingWaits == 1 && !copy->finished && !draw1->finished);
    CHECK(plume::metalSubmissionState().lastSubmitted.empty());
    plume::setMetalAppActive(false);
    CHECK(draw1->schedulingWaits == 1 && copy->schedulingWaits == 1);

    auto* pendingDraw = f.queue.commandBufferWithUnretainedReferences();
    auto* pendingCopy = copyQueue.commandBufferWithUnretainedReferences();
    auto render = std::async(std::launch::async, [=] {
        plume::MetalCommandList list{pendingDraw}; list.commit();
    });
    auto transfer = std::async(std::launch::async, [=] {
        plume::MetalCommandList list{pendingCopy}; list.commit();
    });
    auto acquire = std::async(std::launch::async, [&] { return f.Acquire(); });
    CHECK(render.wait_for(30ms) == std::future_status::timeout);
    CHECK(transfer.wait_for(30ms) == std::future_status::timeout);
    CHECK(acquire.wait_for(30ms) == std::future_status::timeout);
    CHECK(!pendingDraw->enqueued && !pendingCopy->enqueued);
    CHECK(!pendingDraw->committed && !pendingCopy->committed);
    CHECK(f.chain->currentAvailableDrawableIndex == 0);
    CHECK(f.queue.buffers.size() == 3); // Acquisition must not create a buffer.
    plume::setMetalAppActive(true);
    CHECK(render.wait_for(1s) == std::future_status::ready);
    CHECK(transfer.wait_for(1s) == std::future_status::ready);
    CHECK(acquire.wait_for(1s) == std::future_status::ready);
    render.get(); transfer.get();
    const auto index = acquire.get();
    CHECK(pendingDraw->committed && pendingCopy->committed);

    // A native drawable acquired before deactivation waits for activation
    // rather than submitting a present command while the app is inactive.
    plume::setMetalAppActive(false);
    const auto bufferCount = f.queue.buffers.size();
    auto present = std::async(std::launch::async, [&] { f.Present(index); });
    CHECK(present.wait_for(30ms) == std::future_status::timeout);
    CHECK(f.queue.buffers.size() == bufferCount);
    plume::setMetalAppActive(true);
    CHECK(present.wait_for(1s) == std::future_status::ready);
    present.get();
    plume::setMetalAppActive(false);
    copyQueue.FinishAll();
    for (auto& buffer : copyQueue.buffers) CHECK(buffer->references == 0);
}

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    const std::string_view test = argv[1];
    if (test == "ownership") Ownership();
    else if (test == "unavailable") Unavailable();
    else if (test == "callbacks") Callbacks();
    else if (test == "diagnostics") Diagnostics();
    else if (test == "lifecycle") Lifecycle();
    else CHECK(false);
    std::printf("Metal presentation %s passed with -O2 -DNDEBUG\n", argv[1]);
}
