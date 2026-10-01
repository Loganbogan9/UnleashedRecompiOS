#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace launch_request
{
    enum class Action { None, Install, InstallDLC };

    // iOS cannot relaunch its executable with new command-line arguments. Persist
    // only the supported installer actions for the next launch from the home screen.
    inline bool Save(const std::filesystem::path& directory, Action action, std::error_code& error)
    {
        error.clear();
        const auto path = directory / "next-launch.txt";
        if (action == Action::None)
        {
            std::filesystem::remove(path, error);
            return !error;
        }
        std::filesystem::create_directories(directory, error);
        if (error)
            return false;
        auto temporary = path;
        temporary += ".tmp";
        std::ofstream file(temporary, std::ios::trunc);
        file << (action == Action::InstallDLC ? "install-dlc\n" : "install\n");
        file.close();
        if (!file)
        {
            error = std::make_error_code(std::errc::io_error);
            return false;
        }
        std::filesystem::rename(temporary, path, error);
        return !error;
    }

    inline Action Consume(const std::filesystem::path& directory, std::error_code& error)
    {
        error.clear();
        const auto path = directory / "next-launch.txt";
        if (!std::filesystem::exists(path, error) || error)
            return Action::None;
        std::ifstream file(path);
        if (!file.is_open())
        {
            error = std::make_error_code(std::errc::io_error);
            return Action::None;
        }
        // Bound the read so this optional marker can never create a large allocation.
        char text[32]{};
        file.getline(text, sizeof(text));
        const bool validRead = !file.fail();
        file.close();
        Action action = Action::None;
        if (validRead && std::string_view(text) == "install-dlc")
            action = Action::InstallDLC;
        else if (validRead && std::string_view(text) == "install")
            action = Action::Install;
        std::filesystem::remove(path, error);
        if (!error && action == Action::None)
            error = std::make_error_code(std::errc::invalid_argument);
        return error ? Action::None : action;
    }
}
