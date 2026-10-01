#include <os/logger.h>
#include <os/bounded_log_file.h>
#include <os/macos/ios_diagnostics.h>
#include <os/process.h>
#include <user/paths.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <string_view>
#include <chrono>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <os/proc.h>
#include <sys/sysctl.h>
#include <unistd.h>
#endif

static std::mutex s_logMutex;

#if defined(__APPLE__) && TARGET_OS_IPHONE
static os::logger::BoundedLogFile s_logFile;
static bool s_logInitialized = false;
static const auto s_logStart = std::chrono::steady_clock::now();
#endif

static void SafeLogPrint(const std::string_view str, const char* func)
{
    std::lock_guard lock(s_logMutex);

    try
    {
        if (func)
        {
            fmt::println("[{}] {}", func, str);
        }
        else
        {
            fmt::println("{}", str);
        }
    }
    catch (...)
    {
        if (func)
        {
            std::fprintf(stderr, "[%s] %.*s\n", func, static_cast<int>(str.size()), str.data());
        }
        else
        {
            std::fprintf(stderr, "%.*s\n", static_cast<int>(str.size()), str.data());
        }
    }

#if defined(__APPLE__) && TARGET_OS_IPHONE
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_logStart).count();
    s_logFile.Write(func ? fmt::format("[{:9.3f}s] [{}] {}\n", elapsed, func, str)
                        : fmt::format("[{:9.3f}s] {}\n", elapsed, str));
#endif
}

void os::logger::Init()
{
#if defined(__APPLE__) && TARGET_OS_IPHONE
    {
        std::lock_guard lock(s_logMutex);
        if (s_logInitialized)
            return;
        s_logInitialized = true;

        const auto logPath = GetUserPath() / "unleashedrecomp.log";
        std::error_code ec;
        std::filesystem::create_directories(logPath.parent_path(), ec);
        if (ec || !s_logFile.Open(logPath))
            std::fprintf(stderr, "Failed to open iOS diagnostic log: %s\n", logPath.c_str());
        else
            s_logFile.Write("\n--- UnleashedRecomp log start ---\n");
    }

    int mib[] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    kinfo_proc info{};
    size_t infoSize = sizeof(info);
    const bool hasDebuggerInfo = sysctl(mib, 4, &info, &infoSize, nullptr, 0) == 0 && infoSize == sizeof(info);
    Log(fmt::format("iOS launch: debugger={}, pageSize={}, executable={}, cwd={}",
        hasDebuggerInfo ? ((info.kp_proc.p_flag & P_TRACED) ? "attached" : "detached") : "unknown",
        sysconf(_SC_PAGESIZE), os::process::GetExecutablePath().string(), os::process::GetWorkingDirectory().string()));
#if defined(_DEBUG) && _DEBUG
    Log("Build configuration: Debug; CPU recompilation is AOT; guest memory is non-executable.");
#else
    Log("Build configuration: optimized; CPU recompilation is AOT; guest memory is non-executable.");
#endif

    if (CFBundleRef bundle = CFBundleGetMainBundle())
    {
        const auto logBundleValue = [&](CFStringRef key)
        {
            auto value = CFBundleGetValueForInfoDictionaryKey(bundle, key);
            if (value == nullptr || CFGetTypeID(value) != CFStringGetTypeID())
                return;
            char keyText[128]{};
            char valueText[512]{};
            if (CFStringGetCString(key, keyText, sizeof(keyText), kCFStringEncodingUTF8) &&
                CFStringGetCString(static_cast<CFStringRef>(value), valueText, sizeof(valueText), kCFStringEncodingUTF8))
                Log(fmt::format("Bundle {}: {}", keyText, valueText));
        };
        logBundleValue(CFSTR("CFBundleIdentifier"));
        logBundleValue(CFSTR("CFBundleVersion"));
        logBundleValue(CFSTR("DTSDKName"));
    }
    for (const char* variable : { "MTL_DEBUG_LAYER", "METAL_DEVICE_WRAPPER_TYPE", "MTL_HUD_ENABLED" })
    {
        if (const char* value = std::getenv(variable))
            Log(fmt::format("{}={}", variable, value));
    }
    LogRuntimeDiagnostics("logger initialized");
#endif
}

#if defined(__APPLE__) && TARGET_OS_IPHONE
void os::logger::LogRuntimeDiagnostics(std::string_view stage)
{
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS &&
        count >= TASK_VM_INFO_REV1_COUNT)
    {
        constexpr double MiB = 1024.0 * 1024.0;
        Log(fmt::format("Startup [{}]: footprint={:.1f} MiB, resident={:.1f} MiB, available={:.1f} MiB",
            stage, info.phys_footprint / MiB, info.resident_size / MiB, os_proc_available_memory() / MiB));
    }
    else
        Log(fmt::format("Startup [{}]: memory statistics unavailable", stage));
}
#endif

void os::logger::Log(const std::string_view str, ELogType type, const char* func)
{
    (void)type;
    SafeLogPrint(str, func);
}
