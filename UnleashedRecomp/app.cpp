#include "app.h"
#include <api/SWA.h>
#include <gpu/video.h>
#include <install/installer.h>
#include <kernel/function.h>
#include <os/process.h>
#include <os/logger.h>
#include <patches/audio_patches.h>
#include <patches/inspire_patches.h>
#include <ui/game_window.h>
#include <user/config.h>
#include <user/paths.h>
#include <user/registry.h>
#include <user/launch_request.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

void App::Restart(std::vector<std::string> restartArgs)
{
#if defined(__APPLE__) && TARGET_OS_IPHONE
    auto action = launch_request::Action::None;
    for (const auto& arg : restartArgs)
    {
        if (arg == "--install-dlc")
            action = launch_request::Action::InstallDLC;
        else if (arg == "--install")
            action = launch_request::Action::Install;
        else
        {
            LOGFN_ERROR("Unsupported iOS restart argument: {}", arg);
            return;
        }
    }
    std::error_code error;
    if (!launch_request::Save(GetUserPath(), action, error))
    {
        LOGFN_ERROR("Failed to save next-launch action: {}", error.message());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GameWindow::GetTitle(), "Could not save the restart request. Check available storage and try again.", GameWindow::s_pWindow);
        return;
    }
    LOGN("iOS restart requested; settings and installer action will apply on the next home-screen launch.");
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, GameWindow::GetTitle(), "Open Unleashed again from the home screen to continue.", GameWindow::s_pWindow);
#else
    os::process::StartProcess(os::process::GetExecutablePath(), restartArgs, os::process::GetWorkingDirectory());
#endif
    Exit();
}

void App::Exit()
{
    Config::Save();

#ifdef _WIN32
    timeEndPeriod(1);
#endif

    std::_Exit(0);
}

// SWA::CApplication::CApplication
PPC_FUNC_IMPL(__imp__sub_824EB490);
PPC_FUNC(sub_824EB490)
{
    App::s_isInit = true;
    App::s_isMissingDLC = !Installer::checkAllDLC(GetGamePath());
    App::s_language = Config::Language;

    SWA::SGlobals::Init();
    Registry::Save();

    __imp__sub_824EB490(ctx, base);
}

static std::thread::id g_mainThreadId = std::this_thread::get_id();

// SWA::CApplication::Update
PPC_FUNC_IMPL(__imp__sub_822C1130);
PPC_FUNC(sub_822C1130)
{
    Video::WaitOnSwapChain();

    // Correct small delta time errors.
    if (Config::FPS >= FPS_MIN && Config::FPS < FPS_MAX)
    {
        double targetDeltaTime = 1.0 / Config::FPS;

        if (abs(ctx.f1.f64 - targetDeltaTime) < 0.00001)
            ctx.f1.f64 = targetDeltaTime;
    }

    App::s_deltaTime = ctx.f1.f64;
    App::s_time += App::s_deltaTime;

    // This function can also be called by the loading thread,
    // which SDL does not like. To prevent the OS from thinking
    // the process is unresponsive, we will flush while waiting
    // for the pipelines to finish compiling in video.cpp.
    if (std::this_thread::get_id() == g_mainThreadId)
    {
        SDL_PumpEvents();
        SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
        GameWindow::Update();
    }

    AudioPatches::Update(App::s_deltaTime);
    InspirePatches::Update();

    // Apply subtitles option.
    if (auto pApplicationDocument = SWA::CApplicationDocument::GetInstance())
        pApplicationDocument->m_InspireSubtitles = Config::Subtitles;

    if (Config::EnableEventCollisionDebugView)
        *SWA::SGlobals::ms_IsTriggerRender = true;

    if (Config::EnableGIMipLevelDebugView)
        *SWA::SGlobals::ms_VisualizeLoadedLevel = true;

    if (Config::EnableObjectCollisionDebugView)
        *SWA::SGlobals::ms_IsObjectCollisionRender = true;

    if (Config::EnableStageCollisionDebugView)
        *SWA::SGlobals::ms_IsCollisionRender = true;

    __imp__sub_822C1130(ctx, base);
}
