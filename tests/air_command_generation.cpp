#include <air_compiler.h>
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <spawn.h>
#include <string>
#include <vector>
#include <unistd.h>

static const std::string shader = "complete generated shader source";
static std::vector<std::filesystem::path> temporaryFiles;
static unsigned commands = 0;
static unsigned waits = 0;
static unsigned writes = 0;

extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" ssize_t __wrap_write(int fd, const void* data, size_t size)
{
    if (writes++ == 0)
    {
        errno = EINTR;
        return -1;
    }
    return __real_write(fd, data, std::min(size, size_t(3)));
}

extern "C" int __wrap_posix_spawn(pid_t* pid, const char*, const posix_spawn_file_actions_t*,
    const posix_spawnattr_t*, char* const* argv, char* const* environment)
{
    assert(environment != nullptr);
    bool inherited = false;
    for (size_t i = 0; environment[i] != nullptr; ++i)
        inherited |= std::string(environment[i]) == "DEVELOPER_DIR=/test/selected Xcode";
    assert(inherited);
    std::vector<std::string> args;
    for (size_t i = 0; argv[i] != nullptr; ++i)
        args.emplace_back(argv[i]);
    assert(args[1] == "-sdk" && args[2] == XENOS_RECOMP_AIR_SDK);
    const auto value = [&](const std::string& flag) -> const std::string&
    {
        auto found = std::find(args.begin(), args.end(), flag);
        assert(found != args.end() && found + 1 != args.end());
        return *(found + 1);
    };
    const auto output = value("-o");
    if (commands++ == 0)
    {
        assert(args[3] == "metal" && value("-target") == XENOS_RECOMP_AIR_TARGET);
        const auto input = value("-c");
        std::ifstream source(input);
        const std::string contents((std::istreambuf_iterator<char>(source)), {});
        assert(contents == shader);
        temporaryFiles.emplace_back(input);
        std::ofstream(output) << XENOS_RECOMP_AIR_SDK << ':' << XENOS_RECOMP_AIR_TARGET;
    }
    else
    {
        assert(args[3] == "metallib");
        std::filesystem::copy_file(args.back(), output);
    }
    temporaryFiles.emplace_back(output);
    *pid = 123;
    return 0;
}

extern "C" pid_t __wrap_waitpid(pid_t pid, int* status, int)
{
    if (waits++ == 0)
    {
        errno = EINTR;
        return -1;
    }
    *status = 0;
    return pid;
}

int main()
{
    assert(setenv("DEVELOPER_DIR", "/test/selected Xcode", 1) == 0);
    const auto bytes = AirCompiler::compile(shader);
    assert(std::string(bytes.begin(), bytes.end()) == std::string(XENOS_RECOMP_AIR_SDK) + ':' + XENOS_RECOMP_AIR_TARGET);
    assert(commands == 2 && waits == 3 && writes > 2);
    for (const auto& path : temporaryFiles)
        assert(!std::filesystem::exists(path));
}
