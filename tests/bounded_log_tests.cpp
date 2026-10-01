#include <os/bounded_log_file.h>
#include <cassert>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

std::string Read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}

int main()
{
    const auto directory = std::filesystem::temp_directory_path() / ("unleashed-log-test-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);
    const auto path = directory / "diagnostic.log";
    auto previous = path;
    previous += ".previous";
    {
        os::logger::BoundedLogFile file;
        assert(file.Open(path, 12));
        assert(file.Write("first line\n"));
        assert(Read(path) == "first line\n"); // Flushes before termination.
        assert(file.Write("next line\n"));
        assert(Read(previous) == "first line\n");
        assert(Read(path) == "next line\n");
        assert(file.Write("0123456789abcdef"));
        assert(Read(previous) == "next line\n");
        assert(Read(path) == "0123456789ab"); // Individual oversized entries stay bounded.
    }
    {
        os::logger::BoundedLogFile file;
        assert(file.Open(path, 12));
        assert(file.Write("relaunch\n"));
        assert(Read(previous) == "0123456789ab");
        assert(Read(path) == "relaunch\n");
    }
    {
        os::logger::BoundedLogFile file;
        assert(!file.Open(directory / "missing" / "diagnostic.log"));
        assert(!file.Write("no file"));
    }
    {
        os::logger::BoundedLogFile file;
        assert(!file.Open(path, 0));
    }
    std::filesystem::remove_all(directory);
    std::cout << "Bounded log tests passed\n";
}
