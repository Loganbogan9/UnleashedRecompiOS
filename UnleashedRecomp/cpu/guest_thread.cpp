#include <stdafx.h>
#include "guest_thread.h"
#include <kernel/memory.h>
#include <kernel/heap.h>
#include <kernel/function.h>
#include <os/logger.h>
#include "ppc_context.h"
#include <new>
#include <system_error>
#include <cstring>
#include <cstdlib>
#ifdef USE_PTHREAD
#include <sys/resource.h>
#endif

constexpr size_t PCR_SIZE = 0xAB0;
constexpr size_t TLS_SIZE = 0x100;
constexpr size_t TEB_SIZE = 0x2E0;
constexpr size_t STACK_SIZE = 0x40000;
constexpr size_t TOTAL_SIZE = PCR_SIZE + TLS_SIZE + TEB_SIZE + STACK_SIZE;

constexpr size_t TEB_OFFSET = PCR_SIZE + TLS_SIZE;

GuestThreadContext::GuestThreadContext(uint32_t cpuNumber)
{
    assert(thread == nullptr);

    thread = (uint8_t*)g_userHeap.Alloc(TOTAL_SIZE);
    if (thread == nullptr)
    {
        LOGFN_ERROR("GuestThreadContext failed to allocate {} bytes for CPU {}.", TOTAL_SIZE, cpuNumber);
        std::abort();
    }
    memset(thread, 0, TOTAL_SIZE);

    *(uint32_t*)thread = ByteSwap(g_memory.MapVirtual(thread + PCR_SIZE)); // tls pointer
    *(uint32_t*)(thread + 0x100) = ByteSwap(g_memory.MapVirtual(thread + PCR_SIZE + TLS_SIZE)); // teb pointer
    *(thread + 0x10C) = cpuNumber;

    *(uint32_t*)(thread + PCR_SIZE + 0x10) = 0xFFFFFFFF; // that one TLS entry that felt quirky
    *(uint32_t*)(thread + PCR_SIZE + TLS_SIZE + 0x14C) = ByteSwap(GuestThread::GetCurrentThreadId()); // thread id

    ppcContext.r1.u64 = g_memory.MapVirtual(thread + PCR_SIZE + TLS_SIZE + TEB_SIZE + STACK_SIZE); // stack pointer
    ppcContext.r13.u64 = g_memory.MapVirtual(thread);
    ppcContext.fpscr.loadFromHost();

    assert(GetPPCContext() == nullptr);
    SetPPCContext(ppcContext);
}

GuestThreadContext::~GuestThreadContext()
{
    assert(GetPPCContext() == &ppcContext);
    g_ppcContext = nullptr;
    g_userHeap.Free(thread);
}

template <typename ThreadType>
static uint32_t CalcThreadId(const ThreadType& id)
{
    if constexpr (sizeof(id) == 4)
    {
        uint32_t value;
        std::memcpy(&value, &id, sizeof(value));
        return value;
    }
    else
        return XXH32(&id, sizeof(id), 0);
}

#ifdef USE_PTHREAD
static size_t GetStackSize(uint32_t requestedSize)
{
    // Cache as this should not change.
    static const size_t stackSize = []() -> size_t
    {
        // 8 MiB is a typical default.
        constexpr auto defaultSize = 8 * 1024 * 1024;
        struct rlimit lim;
        const auto ret = getrlimit(RLIMIT_STACK, &lim);
        if (ret == 0 && lim.rlim_cur < defaultSize)
        {
            // Use what the system allows.
            return lim.rlim_cur;
        }
        else
        {
            return defaultSize;
        }
    }();

    size_t targetSize = stackSize;
    if (requestedSize != 0)
    {
        targetSize = std::max(targetSize, static_cast<size_t>(requestedSize));
    }

#if defined(UNLEASHED_RECOMP_IOS)
    constexpr size_t IOS_MIN_GUEST_STACK = 16 * 1024 * 1024;
    targetSize = std::max(targetSize, IOS_MIN_GUEST_STACK);
#endif

    targetSize = std::max(targetSize, static_cast<size_t>(PTHREAD_STACK_MIN));
    return targetSize;
}

static void* GuestThreadFunc(void* arg)
{
    GuestThreadHandle* hThread = (GuestThreadHandle*)arg;
#else
static void GuestThreadFunc(GuestThreadHandle* hThread)
{
#endif
    hThread->suspended.wait(true);
    LOGFN("GuestThreadFunc begin - function: 0x{:08X}, value: 0x{:08X}, flags: 0x{:08X}", hThread->params.function, hThread->params.value, hThread->params.flags);
    GuestThread::Start(hThread->params);
    LOGFN("GuestThreadFunc end - function: 0x{:08X}", hThread->params.function);
#ifdef USE_PTHREAD
    return nullptr;
#endif
}

GuestThreadHandle::GuestThreadHandle(const GuestThreadParams& params)
    : params(params), suspended((params.flags & 0x1) != 0)
#ifdef USE_PTHREAD
{
    pthread_attr_t attr;
    creationError = pthread_attr_init(&attr);
    if (creationError != 0)
    {
        LOGFN_ERROR("pthread_attr_init failed with error code {}.", creationError);
        return;
    }

    const auto stackSize = GetStackSize(params.stackSize);
    creationError = pthread_attr_setstacksize(&attr, stackSize);
    if (creationError != 0)
    {
        LOGFN_ERROR("pthread_attr_setstacksize failed with error {}, requested guest stack {}, host stack {}.", creationError, params.stackSize, stackSize);
        pthread_attr_destroy(&attr);
        return;
    }
    creationError = pthread_create(&thread, &attr, GuestThreadFunc, this);
    pthread_attr_destroy(&attr);
    if (creationError != 0) {
        LOGFN_ERROR("pthread_create failed with error {}, requested guest stack {}, host stack {}.", creationError, params.stackSize, stackSize);
        return;
    }

    threadId = CalcThreadId(thread);
    LOGFN("GuestThreadHandle created - function: 0x{:08X}, value: 0x{:08X}, flags: 0x{:08X}, threadId: 0x{:08X}", params.function, params.value, params.flags, GetThreadId());
}
#else
      , thread(GuestThreadFunc, this)
{
    threadId = CalcThreadId(thread.get_id());
}
#endif

GuestThreadHandle::~GuestThreadHandle()
{
    if (creationError == 0)
        Wait(INFINITE);
}

uint32_t GuestThreadHandle::GetThreadId() const
{
    return threadId;
}

uint32_t GuestThreadHandle::Wait(uint32_t timeout)
{
    assert(timeout == INFINITE);
    std::lock_guard lock(waitMutex);
    if (creationError != 0)
        return STATUS_FAIL_CHECK;
    if (joined)
        return STATUS_WAIT_0;

#ifdef USE_PTHREAD
    const int error = pthread_join(thread, nullptr);
    if (error != 0)
    {
        LOGFN_ERROR("pthread_join failed with error {} for thread 0x{:08X}.", error, threadId);
        return STATUS_FAIL_CHECK;
    }
#else
    if (thread.joinable())
        thread.join();
#endif
    joined = true;

    return STATUS_WAIT_0;
}

uint32_t GuestThread::Start(const GuestThreadParams& params)
{
    const auto procMask = (uint8_t)(params.flags >> 24);
    const auto cpuNumber = procMask == 0 ? 0 : 7 - std::countl_zero(procMask);

    LOGFN("GuestThread::Start begin - function: 0x{:08X}, value: 0x{:08X}, flags: 0x{:08X}, cpu: {}", params.function, params.value, params.flags, cpuNumber);
    GuestThreadContext ctx(cpuNumber);
    ctx.ppcContext.r3.u64 = params.value;

    auto* function = g_memory.FindFunction(params.function);
    LOGFN("GuestThread::Start resolved host function - guest: 0x{:08X}, host: {}", params.function, reinterpret_cast<void*>(function));
    function(ctx.ppcContext, g_memory.base);

    LOGFN("GuestThread::Start end - function: 0x{:08X}, r3: 0x{:08X}", params.function, ctx.ppcContext.r3.u32);
    return ctx.ppcContext.r3.u32;
}

GuestThreadHandle* GuestThread::Start(const GuestThreadParams& params, uint32_t* threadId)
{
    void* storage = g_userHeap.AllocPhysical(sizeof(GuestThreadHandle), alignof(GuestThreadHandle));
    if (storage == nullptr)
    {
        LOGFN_ERROR("Failed to allocate GuestThreadHandle for function 0x{:08X}.", params.function);
        return nullptr;
    }

    GuestThreadHandle* hThread;
    try
    {
        hThread = new (storage) GuestThreadHandle(params);
    }
    catch (const std::system_error& error)
    {
        LOGFN_ERROR("Native thread creation failed for function 0x{:08X}: {}.", params.function, error.what());
        g_userHeap.Free(storage);
        return nullptr;
    }

    if (hThread->creationError != 0)
    {
        hThread->~GuestThreadHandle();
        g_userHeap.Free(storage);
        return nullptr;
    }

    if (threadId != nullptr)
    {
        *threadId = hThread->GetThreadId();
    }

    return hThread;
}

uint32_t GuestThread::GetCurrentThreadId()
{
#ifdef USE_PTHREAD
    return CalcThreadId(pthread_self());
#else
    return CalcThreadId(std::this_thread::get_id());
#endif
}

void GuestThread::SetLastError(uint32_t error)
{
    auto* thread = (char*)g_memory.Translate(GetPPCContext()->r13.u32);
    if (*(uint32_t*)(thread + 0x150))
    {
        // Program doesn't want errors
        return;
    }

    // TEB + 0x160 : Win32LastError
    *(uint32_t*)(thread + TEB_OFFSET + 0x160) = ByteSwap(error);
}

#ifdef _WIN32
void GuestThread::SetThreadName(uint32_t threadId, const char* name)
{
#pragma pack(push,8)
    const DWORD MS_VC_EXCEPTION = 0x406D1388;

    typedef struct tagTHREADNAME_INFO
    {
        DWORD dwType; // Must be 0x1000.
        LPCSTR szName; // Pointer to name (in user addr space).
        DWORD dwThreadID; // Thread ID (-1=caller thread).
        DWORD dwFlags; // Reserved for future use, must be zero.
    } THREADNAME_INFO;
#pragma pack(pop)

    THREADNAME_INFO info;
    info.dwType = 0x1000;
    info.szName = name;
    info.dwThreadID = threadId;
    info.dwFlags = 0;

    __try
    {
        RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}
#endif

void SetThreadNameImpl(uint32_t a1, uint32_t threadId, uint32_t* name)
{
#ifdef _WIN32
    GuestThread::SetThreadName(threadId, (const char*)g_memory.Translate(ByteSwap(*name)));
#endif
}

int GetThreadPriorityImpl(GuestThreadHandle* hThread)
{
#ifdef _WIN32
    return GetThreadPriority(hThread == GetKernelObject(CURRENT_THREAD_HANDLE) ? GetCurrentThread() : hThread->thread.native_handle());
#else 
    return 0;
#endif
}

uint32_t SetThreadIdealProcessorImpl(GuestThreadHandle* hThread, uint32_t dwIdealProcessor)
{
    return 0;
}

GUEST_FUNCTION_HOOK(sub_82DFA2E8, SetThreadNameImpl);
GUEST_FUNCTION_HOOK(sub_82BD57A8, GetThreadPriorityImpl);
GUEST_FUNCTION_HOOK(sub_82BD5910, SetThreadIdealProcessorImpl);

GUEST_FUNCTION_STUB(sub_82BD58F8); // Some function that updates the TEB, don't really care since the field is not set
