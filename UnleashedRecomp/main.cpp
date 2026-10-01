#include <stdafx.h>
#ifdef __x86_64__
#include <cpuid.h>
#endif
#include <cpu/guest_thread.h>
#include <gpu/video.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <kernel/heap.h>
#include <kernel/xam.h>
#include <kernel/io/file_system.h>
#include <file.h>
#include <xex.h>
#include <apu/audio.h>
#include <hid/hid.h>
#include <user/config.h>
#include <user/paths.h>
#include <user/persistent_storage_manager.h>
#include <user/registry.h>
#include <user/launch_request.h>
#include <kernel/xdbf.h>
#include <kernel/xex_load.h>
#include <install/installer.h>
#include <install/update_checker.h>
#include <os/logger.h>
#include <os/macos/ios_diagnostics.h>
#include <os/process.h>
#include <os/registry.h>
#include <ui/game_window.h>
#include <ui/installer_wizard.h>
#include <mod/mod_loader.h>
#include <preload_executable.h>
#include <SDL.h>

#ifdef _WIN32
#include <timeapi.h>
#endif

#if defined(_WIN32) && defined(UNLEASHED_RECOMP_D3D12)
static std::array<std::string_view, 3> g_D3D12RequiredModules =
{
    "D3D12/D3D12Core.dll",
    "dxcompiler.dll",
    "dxil.dll"
};
#endif

const size_t XMAIOBegin = 0x7FEA0000;
const size_t XMAIOEnd = XMAIOBegin + 0x0000FFFF;

Memory g_memory;
Heap g_userHeap;
XDBFWrapper g_xdbfWrapper;
std::unordered_map<uint16_t, GuestTexture*> g_xdbfTextureCache;

void HostStartup()
{
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    hid::Init();
}

// Name inspired from nt's entry point
void KiSystemStartup()
{
    if (g_memory.base == nullptr)
    {
        LOGFN_ERROR("Failed to initialize the 4 GiB guest memory space: stage {}, native error {}.",
            g_memory.initializationFailureStage, g_memory.initializationNativeError);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GameWindow::GetTitle(), Localise("System_MemoryAllocationFailed").c_str(), GameWindow::s_pWindow);
        std::_Exit(1);
    }

    LOGFN("Guest memory base: {}", static_cast<void*>(g_memory.base));
    os::logger::LogRuntimeDiagnostics("guest heap initialization begin");
    g_userHeap.Init();
    os::logger::LogRuntimeDiagnostics("guest heap initialization complete");

    const auto gameContent = XamMakeContent(XCONTENTTYPE_RESERVED, "Game");
    const auto updateContent = XamMakeContent(XCONTENTTYPE_RESERVED, "Update");
    const std::string gamePath = (const char*)(GetGamePath() / "game").u8string().c_str();
    const std::string updatePath = (const char*)(GetGamePath() / "update").u8string().c_str();
    XamRegisterContent(gameContent, gamePath);
    XamRegisterContent(updateContent, updatePath);

    const auto saveFilePath = GetSaveFilePath(true);
    bool saveFileExists = std::filesystem::exists(saveFilePath);

    if (!saveFileExists)
    {
        // Copy base save data to modded save as fallback.
        std::error_code ec;
        std::filesystem::create_directories(saveFilePath.parent_path(), ec);

        if (!ec)
        {
            std::filesystem::copy_file(GetSaveFilePath(false), saveFilePath, ec);
            saveFileExists = !ec;
        }
    }

    if (saveFileExists)
    {
        std::u8string savePathU8 = saveFilePath.parent_path().u8string();
        XamRegisterContent(XamMakeContent(XCONTENTTYPE_SAVEDATA, "SYS-DATA"), (const char*)(savePathU8.c_str()));
    }

    // Mount game
    XamContentCreateEx(0, "game", &gameContent, OPEN_EXISTING, nullptr, nullptr, 0, 0, nullptr);
    XamContentCreateEx(0, "update", &updateContent, OPEN_EXISTING, nullptr, nullptr, 0, 0, nullptr);

    // OS mounts game data to D:
    XamContentCreateEx(0, "D", &gameContent, OPEN_EXISTING, nullptr, nullptr, 0, 0, nullptr);

    std::error_code ec;
    for (auto& file : std::filesystem::directory_iterator(GetGamePath() / "dlc", ec))
    {
        if (file.is_directory())
        {
            std::u8string fileNameU8 = file.path().filename().u8string();
            std::u8string filePathU8 = file.path().u8string();
            XamRegisterContent(XamMakeContent(XCONTENTTYPE_DLC, (const char*)(fileNameU8.c_str())), (const char*)(filePathU8.c_str()));
        }
    }

    XAudioInitializeSystem();
}

uint32_t LdrLoadModule(const std::filesystem::path &path)
{
    auto loadResult = LoadFile(path);
    if (loadResult.empty())
    {
        LOGFN_ERROR("Failed to load module: {}", (const char*)path.u8string().c_str());
        return 0;
    }

    xex_load::Image image;
    std::string_view error;
    if (!xex_load::Validate(loadResult, PPC_MEMORY_SIZE, image, error, g_memory.guardPageSize))
    {
        LOGFN_ERROR("Invalid module '{}': {}", (const char*)path.u8string().c_str(), error);
        return 0;
    }

    auto srcData = image.data.data();
    auto destData = reinterpret_cast<uint8_t*>(g_memory.Translate(image.loadAddress));
    if (image.basicBlocks.empty())
        memcpy(destData, srcData, image.imageSize);
    else
    {
        for (size_t offset = 0; offset < image.basicBlocks.size(); offset += sizeof(Xex2FileBasicCompressionBlock))
        {
            const auto block = xex_load::Read<Xex2FileBasicCompressionBlock>(image.basicBlocks, offset);
            memcpy(destData, srcData, block.dataSize);

            srcData += block.dataSize;
            destData += block.dataSize;

            memset(destData, 0, block.zeroSize);
            destData += block.zeroSize;
        }
    }

    g_xdbfWrapper = XDBFWrapper((uint8_t*)g_memory.Translate(image.resourceAddress), image.resourceSize);

    return image.entryPoint;
}

#ifdef __x86_64__
__attribute__((constructor(101), target("no-avx,no-avx2"), noinline))
void init()
{
    uint32_t eax, ebx, ecx, edx;

    // Execute CPUID for processor info and feature bits.
    __get_cpuid(1, &eax, &ebx, &ecx, &edx);

    // Check for AVX support.
    if ((ecx & (1 << 28)) == 0)
    {
        printf("[*] CPU does not support the AVX instruction set.\n");

#ifdef _WIN32
        MessageBoxA(nullptr, "Your CPU does not meet the minimum system requirements.", "Unleashed Recompiled", MB_ICONERROR);
#endif

        std::_Exit(1);
    }
}
#endif

int main(int argc, char *argv[])
{
#ifdef _WIN32
    timeBeginPeriod(1);
#endif

    os::process::CheckConsole();

    os::logger::Init();

    // The reservation occurs during static initialization. Record its failure
    // before renderer/installer startup can obscure a pre-existing VM limit.
    if (g_memory.base == nullptr)
        LOGFN_ERROR("Guest memory unavailable at process start: stage {}, native error {}.",
            g_memory.initializationFailureStage, g_memory.initializationNativeError);

    if (!os::registry::Init())
        LOGN_WARNING("OS does not support registry.");

    PreloadContext preloadContext;
    preloadContext.PreloadExecutable();

    bool forceInstaller = false;
    bool forceDLCInstaller = false;
    bool useDefaultWorkingDirectory = false;
    bool forceInstallationCheck = false;
    bool graphicsApiRetry = false;
    const char *sdlVideoDriver = nullptr;

    for (uint32_t i = 1; i < argc; i++)
    {
        forceInstaller = forceInstaller || (strcmp(argv[i], "--install") == 0);
        forceDLCInstaller = forceDLCInstaller || (strcmp(argv[i], "--install-dlc") == 0);
        useDefaultWorkingDirectory = useDefaultWorkingDirectory || (strcmp(argv[i], "--use-cwd") == 0);
        forceInstallationCheck = forceInstallationCheck || (strcmp(argv[i], "--install-check") == 0);
        graphicsApiRetry = graphicsApiRetry || (strcmp(argv[i], "--graphics-api-retry") == 0);

        if (strcmp(argv[i], "--sdl-video-driver") == 0)
        {
            if ((i + 1) < argc)
                sdlVideoDriver = argv[++i];
            else
                LOGN_WARNING("No argument was specified for --sdl-video-driver. Option will be ignored.");
        }
    }

    if (!useDefaultWorkingDirectory)
    {
        // Set the current working directory to the executable's path.
        std::error_code ec;
        std::filesystem::current_path(os::process::GetExecutableRoot(), ec);
        if (ec)
            LOGFN_WARNING("Failed to set working directory: {}", ec.message());
    }

    os::logger::LogRuntimeDiagnostics("configuration load begin");
    Config::Load();
    os::logger::LogRuntimeDiagnostics("configuration load complete");

#if defined(__APPLE__) && TARGET_OS_IPHONE
    std::error_code launchError;
    const auto nextAction = launch_request::Consume(GetUserPath(), launchError);
    if (launchError)
        LOGFN_WARNING("Could not consume next-launch action: {}", launchError.message());
    forceInstaller = forceInstaller || nextAction == launch_request::Action::Install;
    forceDLCInstaller = forceDLCInstaller || nextAction == launch_request::Action::InstallDLC;
    if (nextAction != launch_request::Action::None)
        LOGN("Applying saved iOS installer action for this launch.");
#endif

    LOGFN("Resolved user path: {}", (const char*)GetUserPath().u8string().c_str());
    LOGFN("Resolved game path: {}", (const char*)GetGamePath().u8string().c_str());

    if (forceInstallationCheck)
    {
        // Create the console to show progress to the user, otherwise it will seem as if the game didn't boot at all.
        os::process::ShowConsole();

        Journal journal;
        double lastProgressMiB = 0.0;
        double lastTotalMib = 0.0;
        Installer::checkInstallIntegrity(GetGamePath(), journal, [&]()
        {
            constexpr double MiBDivisor = 1024.0 * 1024.0;
            constexpr double MiBProgressThreshold = 128.0;
            double progressMiB = double(journal.progressCounter) / MiBDivisor;
            double totalMiB = double(journal.progressTotal) / MiBDivisor;
            if (journal.progressCounter > 0)
            {
                if ((progressMiB - lastProgressMiB) > MiBProgressThreshold)
                {
                    fprintf(stdout, "Checking files: %0.2f MiB / %0.2f MiB\n", progressMiB, totalMiB);
                    lastProgressMiB = progressMiB;
                }
            }
            else
            {
                if ((totalMiB - lastTotalMib) > MiBProgressThreshold)
                {
                    fprintf(stdout, "Scanning files: %0.2f MiB\n", totalMiB);
                    lastTotalMib = totalMiB;
                }
            }

            return true;
        });

        char resultText[512];
        uint32_t messageBoxStyle;
        if (journal.lastResult == Journal::Result::Success)
        {
            snprintf(resultText, sizeof(resultText), "%s", Localise("IntegrityCheck_Success").c_str());
            fprintf(stdout, "%s\n", resultText);
            messageBoxStyle = SDL_MESSAGEBOX_INFORMATION;
        }
        else
        {
            snprintf(resultText, sizeof(resultText), Localise("IntegrityCheck_Failed").c_str(), journal.lastErrorMessage.c_str());
            fprintf(stderr, "%s\n", resultText);
            messageBoxStyle = SDL_MESSAGEBOX_ERROR;
        }

        SDL_ShowSimpleMessageBox(messageBoxStyle, GameWindow::GetTitle(), resultText, GameWindow::s_pWindow);
        std::_Exit(int(journal.lastResult));
    }

#if defined(_WIN32) && defined(UNLEASHED_RECOMP_D3D12)
    for (auto& dll : g_D3D12RequiredModules)
    {
        if (!std::filesystem::exists(g_executableRoot / dll))
        {
            char text[512];
            snprintf(text, sizeof(text), Localise("System_Win32_MissingDLLs").c_str(), dll.data());
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GameWindow::GetTitle(), text, GameWindow::s_pWindow);
            std::_Exit(1);
        }
    }
#endif

    // Check the time since the last time an update was checked. Store the new time if the difference is more than six hours.
    constexpr double TimeBetweenUpdateChecksInSeconds = 6 * 60 * 60;
    time_t timeNow = std::time(nullptr);
    double timeDifferenceSeconds = difftime(timeNow, Config::LastChecked);
    if (timeDifferenceSeconds > TimeBetweenUpdateChecksInSeconds)
    {
        UpdateChecker::initialize();
        UpdateChecker::start();
        Config::LastChecked = timeNow;
        Config::Save();
    }

    if (Config::ShowConsole)
        os::process::ShowConsole();

    os::logger::LogRuntimeDiagnostics("host startup begin");
    HostStartup();
    os::logger::LogRuntimeDiagnostics("host startup complete");

    const std::filesystem::path gameRoot = GetGamePath();
    const std::filesystem::path patchedExecutablePath = gameRoot / "patched" / "default.xex";
    const std::filesystem::path updatePatchPath = gameRoot / "update" / "default.xexp";
    const std::filesystem::path gameExecutablePath = gameRoot / "game" / "default.xex";
    const bool hasPatchedExecutable = std::filesystem::exists(patchedExecutablePath);
    const bool hasUpdatePatch = std::filesystem::exists(updatePatchPath);
    const bool hasGameExecutable = std::filesystem::exists(gameExecutablePath);
    LOGFN("Install files present - patched/default.xex: {}, update/default.xexp: {}, game/default.xex: {}", hasPatchedExecutable, hasUpdatePatch, hasGameExecutable);

    std::filesystem::path modulePath;
    bool isGameInstalled = Installer::checkGameInstall(gameRoot, modulePath);
    bool runInstallerWizard = forceInstaller || forceDLCInstaller || !isGameInstalled;
    LOGFN("Install state - gameInstalled: {}, runInstallerWizard: {}, candidateModulePath: {}", isGameInstalled, runInstallerWizard, (const char*)modulePath.u8string().c_str());
    if (runInstallerWizard)
    {
        if (!Video::CreateHostDevice(sdlVideoDriver, graphicsApiRetry))
        {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GameWindow::GetTitle(), Localise("Video_BackendError").c_str(), GameWindow::s_pWindow);
            std::_Exit(1);
        }

        if (!InstallerWizard::Run(GetGamePath(), isGameInstalled && forceDLCInstaller))
        {
            std::_Exit(0);
        }

        os::logger::LogRuntimeDiagnostics("installer shutdown complete; graphics device retained");

        isGameInstalled = Installer::checkGameInstall(gameRoot, modulePath);
        LOGFN("Post-installer state - gameInstalled: {}, modulePath: {}", isGameInstalled, (const char*)modulePath.u8string().c_str());
        if (!isGameInstalled)
        {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GameWindow::GetTitle(), "Install data is still incomplete after installer. Ensure game/update sources are selected and installation finishes.", GameWindow::s_pWindow);
            std::_Exit(1);
        }
    }

    os::logger::LogRuntimeDiagnostics("mod initialization begin");
    ModLoader::Init();
    os::logger::LogRuntimeDiagnostics("mod initialization complete");

    os::logger::LogRuntimeDiagnostics("persistent storage load begin");
    if (!PersistentStorageManager::LoadBinary())
        LOGFN_ERROR("Failed to load persistent storage binary... (status code {})", (int)PersistentStorageManager::BinStatus);
    os::logger::LogRuntimeDiagnostics("persistent storage load complete");

    LOGN("Starting guest system initialization.");
    KiSystemStartup();
    os::logger::LogRuntimeDiagnostics("guest system initialization complete");

    LOGFN("Loading module: {}", (const char*)modulePath.u8string().c_str());
    uint32_t entry = LdrLoadModule(modulePath);
    os::logger::LogRuntimeDiagnostics("module load complete");
    if (entry == 0)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GameWindow::GetTitle(), "Failed to load game executable (patched/default.xex). Re-run installer and verify game/update files.", GameWindow::s_pWindow);
        std::_Exit(1);
    }

    if (!runInstallerWizard)
    {
        if (!Video::CreateHostDevice(sdlVideoDriver, graphicsApiRetry))
        {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GameWindow::GetTitle(), Localise("Video_BackendError").c_str(), GameWindow::s_pWindow);
            std::_Exit(1);
        }
    }

    Video::StartPipelinePrecompilation();
    os::logger::LogRuntimeDiagnostics("pipeline precompilation launched; entering guest");

    LOGFN("Starting guest thread at entry: 0x{:08X}", entry);
    GuestThread::Start({ entry, 0, 0 });

    return 0;
}

GUEST_FUNCTION_STUB(__imp__vsprintf);
GUEST_FUNCTION_STUB(__imp___vsnprintf);
GUEST_FUNCTION_STUB(__imp__sprintf);
GUEST_FUNCTION_STUB(__imp___snprintf);
GUEST_FUNCTION_STUB(__imp___snwprintf);
GUEST_FUNCTION_STUB(__imp__vswprintf);
GUEST_FUNCTION_STUB(__imp___vscwprintf);
GUEST_FUNCTION_STUB(__imp__swprintf);
