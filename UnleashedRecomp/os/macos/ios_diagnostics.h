#pragma once

#include <string_view>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace os::logger
{
#if defined(__APPLE__) && TARGET_OS_IPHONE
    // Capture footprint and remaining process memory at infrequent startup boundaries.
    void LogRuntimeDiagnostics(std::string_view stage);
#else
    inline void LogRuntimeDiagnostics(std::string_view) {}
#endif
}
