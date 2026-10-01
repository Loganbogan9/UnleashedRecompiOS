#include <user/launch_request.h>
#include <cassert>
#include <iostream>
#include <unistd.h>

int main()
{
    using launch_request::Action;
    const auto directory = std::filesystem::temp_directory_path() / ("unleashed-launch-test-" + std::to_string(getpid()));
    std::error_code error;
    assert(launch_request::Consume(directory, error) == Action::None && !error);
    for (const Action action : { Action::Install, Action::InstallDLC })
    {
        assert(launch_request::Save(directory, action, error) && !error);
        assert(launch_request::Consume(directory, error) == action && !error);
        assert(launch_request::Consume(directory, error) == Action::None && !error);
    }
    assert(launch_request::Save(directory, Action::InstallDLC, error));
    assert(launch_request::Save(directory, Action::Install, error));
    assert(launch_request::Consume(directory, error) == Action::Install && !error);
    assert(launch_request::Save(directory, Action::InstallDLC, error));
    assert(launch_request::Save(directory, Action::None, error));
    assert(launch_request::Consume(directory, error) == Action::None && !error);
    for (const std::string& text : { std::string("unknown\n"), std::string(1024, 'a'), std::string() })
    {
        std::ofstream file(directory / "next-launch.txt");
        file << text;
        file.close();
        assert(launch_request::Consume(directory, error) == Action::None && error);
        assert(launch_request::Consume(directory, error) == Action::None && !error);
    }
    // A regular file cannot serve as the app's user directory.
    std::ofstream(directory / "blocked") << "file";
    assert(!launch_request::Save(directory / "blocked", Action::InstallDLC, error) && error);
    std::filesystem::remove_all(directory);
    std::cout << "Launch request tests passed\n";
}
