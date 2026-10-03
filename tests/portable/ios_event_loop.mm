#include <stdafx.h>
#include <ui/ios_event_loop.h>
#include <ui/game_window.h>
#include <hid/hid.h>
#include <os/app_lifecycle.h>
#include <future>
#import <UIKit/UIKit.h>

@implementation UIApplication
+ (UIApplication*)sharedApplication
{
    static UIApplication* application = [UIApplication new];
    return application;
}
@end

NSNotificationName const UIApplicationWillResignActiveNotification = @"TestWillResignActive";
NSNotificationName const UIApplicationDidBecomeActiveNotification = @"TestDidBecomeActive";
NSNotificationName const UIApplicationDidEnterBackgroundNotification = @"TestDidEnterBackground";
NSNotificationName const UIApplicationWillEnterForegroundNotification = @"TestWillEnterForeground";

static bool audioActive = true, metalActive = true;
static unsigned audioChanges, metalChanges;
void XAudioSetAppActive(bool active)
{
    assert(!app_lifecycle::GetExecutionGate().IsActive());
    audioActive = active;
    ++audioChanges;
}
namespace plume
{
void setMetalAppActive(bool active)
{
    assert(!app_lifecycle::GetExecutionGate().IsActive());
    metalActive = active;
    ++metalChanges;
}
}

static bool failRegistration;
static void (*animationCallback)(void*);
static void* animationParameter;

extern "C" int SDL_iPhoneSetAnimationCallback(SDL_Window* window, int interval, void (*callback)(void*), void* parameter)
{
    assert(window == GameWindow::s_pWindow);
    assert(interval == 1);
    if (failRegistration)
        return SDL_SetError("Test animation registration failure");
    animationCallback = callback;
    animationParameter = parameter;
    return 0;
}

static int WindowEvent(void*, SDL_Event* event)
{
    if (event->type == SDL_WINDOWEVENT)
    {
        if (event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
            GameWindow::s_isFocused = true;
        if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST)
            GameWindow::s_isFocused = false;
    }
    return 0;
}

void TestIOSLoopInit()
{
    SDL_AddEventWatch(WindowEvent, nullptr);
    failRegistration = true;
    assert(!StartIOSEventLoop());
    assert(!animationCallback);
    failRegistration = false;
    UIApplication.sharedApplication.applicationState = UIApplicationStateInactive;
    assert(StartIOSEventLoop());
    assert(animationCallback);
    assert(!hid::IsInputAllowed());
    assert(!audioActive && !metalActive && !app_lifecycle::GetExecutionGate().IsActive());
    UIApplication.sharedApplication.applicationState = UIApplicationStateActive;
    [NSNotificationCenter.defaultCenter postNotificationName:UIApplicationDidBecomeActiveNotification object:nil];
    assert(hid::IsInputAllowed());
    assert(audioActive && metalActive && app_lifecycle::GetExecutionGate().IsActive());
}

void TestIOSLoopTick()
{
    const unsigned previousUpdates = GameWindow::updates;
    animationCallback(animationParameter);
    assert(GameWindow::updates == previousUpdates + 1);
}

void TestIOSFocusLifecycle()
{
    using namespace std::chrono_literals;
    UIApplication.sharedApplication.applicationState = UIApplicationStateBackground;
    [NSNotificationCenter.defaultCenter postNotificationName:UIApplicationWillResignActiveNotification object:nil];
    assert(!hid::IsInputAllowed());
    assert(!audioActive && !metalActive && !app_lifecycle::GetExecutionGate().IsActive());
    const auto previousAudioChanges = audioChanges;
    const auto previousMetalChanges = metalChanges;
    [NSNotificationCenter.defaultCenter postNotificationName:UIApplicationDidEnterBackgroundNotification object:nil];
    assert(audioChanges == previousAudioChanges && metalChanges == previousMetalChanges);
    auto worker = std::async(std::launch::async, [] {
        return app_lifecycle::GetExecutionGate().WaitUntilActive();
    });
    assert(worker.wait_for(30ms) == std::future_status::timeout);
    // Entering foreground while still inactive must not release execution.
    UIApplication.sharedApplication.applicationState = UIApplicationStateInactive;
    [NSNotificationCenter.defaultCenter postNotificationName:UIApplicationWillEnterForegroundNotification object:nil];
    assert(!audioActive && !metalActive && !app_lifecycle::GetExecutionGate().IsActive());
    assert(worker.wait_for(30ms) == std::future_status::timeout);
    UIApplication.sharedApplication.applicationState = UIApplicationStateActive;
    [NSNotificationCenter.defaultCenter postNotificationName:UIApplicationDidBecomeActiveNotification object:nil];
    assert(hid::IsInputAllowed());
    assert(audioActive && metalActive && app_lifecycle::GetExecutionGate().IsActive());
    assert(worker.wait_for(1s) == std::future_status::ready);
    assert(worker.get() == 2);
    // Duplicate active notifications must not clear queued output every tick.
    [NSNotificationCenter.defaultCenter postNotificationName:UIApplicationDidBecomeActiveNotification object:nil];
    assert(audioChanges == previousAudioChanges + 1 && metalChanges == previousMetalChanges + 1);
    // The background notification independently closes the gates as well.
    UIApplication.sharedApplication.applicationState = UIApplicationStateBackground;
    [NSNotificationCenter.defaultCenter postNotificationName:UIApplicationDidEnterBackgroundNotification object:nil];
    assert(!audioActive && !metalActive && !app_lifecycle::GetExecutionGate().IsActive());
    assert(!hid::IsInputAllowed());
    UIApplication.sharedApplication.applicationState = UIApplicationStateActive;
    [NSNotificationCenter.defaultCenter postNotificationName:UIApplicationDidBecomeActiveNotification object:nil];
    assert(audioActive && metalActive && app_lifecycle::GetExecutionGate().IsActive());
}
