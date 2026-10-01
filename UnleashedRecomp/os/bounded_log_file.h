#pragma once

#include <cstdio>
#include <filesystem>
#include <string_view>

namespace os::logger
{
    // Keep the current log and one previous log. The caller provides serialization.
    class BoundedLogFile
    {
        FILE* m_file{};
        std::filesystem::path m_path;
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
            auto previous = m_path;
            previous += ".previous";
            std::error_code ec;
            std::filesystem::remove(previous, ec);
            if (!ec)
                std::filesystem::rename(m_path, previous, ec);
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
            m_limit = limit;
            return OpenFile() && (m_size <= m_limit || Rotate());
        }

        bool Write(std::string_view message)
        {
            if (m_file == nullptr)
                return false;
            message = message.substr(0, m_limit);
            if (m_size > m_limit || message.size() > m_limit - m_size)
            {
                if (!Rotate())
                    return false;
            }
            const size_t written = std::fwrite(message.data(), 1, message.size(), m_file);
            m_size += written;
            // Preserve the last completed breadcrumb even after watchdog/jetsam kills.
            return std::fflush(m_file) == 0 && written == message.size();
        }
    };
}
