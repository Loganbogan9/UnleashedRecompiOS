#pragma once

#include <cstdio>
#include <algorithm>
#include <filesystem>
#include <initializer_list>
#include <string_view>

namespace os::logger
{
    // Keep the current log and one previous log. The caller provides serialization.
    class BoundedLogFile
    {
        FILE* m_file{};
        std::filesystem::path m_path;
        std::filesystem::path m_previousPath;
        size_t m_limit{};
        uintmax_t m_size{};

        bool OpenFile()
        {
            std::error_code ec;
            m_size = std::filesystem::file_size(m_path, ec);
            if (ec)
                m_size = 0;
#ifdef _WIN32
            m_file = _wfopen(m_path.c_str(), L"ab");
#else
            m_file = std::fopen(m_path.c_str(), "ab");
#endif
            return m_file != nullptr;
        }

        bool Rotate()
        {
            if (m_file != nullptr)
                std::fclose(m_file);
            m_file = nullptr;
            std::error_code ec;
            std::filesystem::remove(m_previousPath, ec);
            if (!ec)
                std::filesystem::rename(m_path, m_previousPath, ec);
            // A failed rotation must not erase the current diagnostic evidence.
            const bool rotated = !ec;
            const bool opened = OpenFile();
            return rotated && opened;
        }

    public:
        ~BoundedLogFile()
        {
            if (m_file != nullptr)
                std::fclose(m_file);
        }

        BoundedLogFile() = default;
        BoundedLogFile(const BoundedLogFile&) = delete;
        BoundedLogFile& operator=(const BoundedLogFile&) = delete;

        bool Open(const std::filesystem::path& path, size_t limit = 4 * 1024 * 1024)
        {
            if (m_file != nullptr || limit == 0)
                return false;
            m_path = path;
            m_previousPath = path;
            m_previousPath += ".previous";
            m_limit = limit;
            return OpenFile() && (m_size <= m_limit || Rotate());
        }

        bool Write(std::string_view message)
        {
            return Write({message});
        }

        // Write a complete entry without joining fragments into a heap string.
        // Rotation paths are prepared by Open(), before memory pressure occurs.
        bool Write(std::initializer_list<std::string_view> fragments)
        {
            if (m_file == nullptr)
                return false;
            size_t messageSize = 0;
            for (const auto fragment : fragments)
                messageSize += std::min(fragment.size(), m_limit - messageSize);
            if (m_size > m_limit || messageSize > m_limit - m_size)
            {
                if (!Rotate())
                    return false;
            }
            size_t remaining = messageSize;
            for (const auto fragment : fragments)
            {
                const size_t count = std::min(fragment.size(), remaining);
                if (count == 0)
                    continue;
                const size_t written = std::fwrite(fragment.data(), 1, count, m_file);
                m_size += written;
                remaining -= written;
                if (written != count)
                    break;
            }
            // Preserve the last completed breadcrumb even after watchdog/jetsam kills.
            return std::fflush(m_file) == 0 && remaining == 0;
        }
    };
}
