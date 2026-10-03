#include <stdafx.h>
#include "ios_event_loop.h"
#include <ui/game_window.h>
#include <hid/hid.h>
#include <apu/audio.h>
#include <os/app_lifecycle.h>
#include <os/logger.h>
#ifdef UNLEASHED_RECOMP_METAL
#include <plume_metal_lifecycle.h>
#endif
#include <SDL_system.h>
#import <UIKit/UIKit.h>

static void SetInputFocus(bool focused)
{
    if (GameWindow::s_isFocused == focused)
        return;

    // Use the existing focus handler, including stopping controller rumble.
    SDL_Event event{};
    event.type = SDL_WINDOWEVENT;
    event.window.windowID = SDL_GetWindowID(GameWindow::s_pWindow);
    event.window.event = focused ? SDL_WINDOWEVENT_FOCUS_GAINED : SDL_WINDOWEVENT_FOCUS_LOST;
    SDL_PushEvent(&event);
}

static void PumpEvents(void*)
{
    // SDL disables its nested UIKit run-loop pump after SDL_main returns.
    // SDL_PumpEvents still updates joysticks and dispatches our event watches.
    SetInputFocus(UIApplication.sharedApplication.applicationState == UIApplicationStateActive);
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    hid::Update();
    GameWindow::Update();
}

static void SetExecutionActive(bool active)
{
    assert(NSThread.isMainThread);
    auto& gate = app_lifecycle::GetExecutionGate();
    if (gate.IsActive() == active)
        return;

    if (!active)
    {
        gate.SetActive(false);
        XAudioSetAppActive(false);
#ifdef UNLEASHED_RECOMP_METAL
        // This only synchronizes Metal submissions, never guest/UI work.
        // All previously committed buffers must be scheduled before UIKit
        // returns from the background notification.
        plume::setMetalAppActive(false);
#endif
        LOGN("iOS pause requested; audio stopped and Metal submissions drained.");
    }
    else
    {
#ifdef UNLEASHED_RECOMP_METAL
        plume::setMetalAppActive(true);
#endif
        XAudioSetAppActive(true);
        gate.SetActive(true);
        LOGN("iOS execution resumed.");
    }
}

bool StartIOSEventLoop()
{
    assert(NSThread.isMainThread);

    if (SDL_iPhoneSetAnimationCallback(GameWindow::s_pWindow, 1, PumpEvents, nullptr) != 0)
        return false;

    // SDL also removes its application notification observer when SDL_main
    // returns. Own both focus and execution state while the guest runs independently.
    // UIKit posts these on its main thread. A nil queue delivers synchronously
    // so GPU scheduling finishes before the background notification returns.
    static id inactiveObserver = [NSNotificationCenter.defaultCenter
        addObserverForName:UIApplicationWillResignActiveNotification object:nil
        queue:nil usingBlock:^(NSNotification*) {
            SetInputFocus(false);
            SetExecutionActive(false);
        }];
    static id backgroundObserver = [NSNotificationCenter.defaultCenter
        addObserverForName:UIApplicationDidEnterBackgroundNotification object:nil
        queue:nil usingBlock:^(NSNotification*) {
            SetInputFocus(false);
            SetExecutionActive(false);
        }];
    static id activeObserver = [NSNotificationCenter.defaultCenter
        addObserverForName:UIApplicationDidBecomeActiveNotification object:nil
        queue:nil usingBlock:^(NSNotification*) {
            SetInputFocus(true);
            SetExecutionActive(true);
        }];
    (void)inactiveObserver;
    (void)backgroundObserver;
    (void)activeObserver;

    const bool active = UIApplication.sharedApplication.applicationState == UIApplicationStateActive;
    SetInputFocus(active);
    SetExecutionActive(active);
    return true;
}
