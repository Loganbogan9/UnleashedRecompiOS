#include <stdafx.h>
#include <ui/ios_event_loop.h>
#include <ui/ios_touch_controls.h>
#include <ui/game_window.h>
#include <hid/hid.h>
#include <os/app_lifecycle.h>
#include <user/config.h>
#include <future>
#import <UIKit/UIKit.h>
#import <objc/message.h>

static bool failRegistration;
static CADisplayLink* testDisplayLink;
static UIWindow* testWindow;
static unsigned touchUpdates, touchResets;
bool StartIOSTouchControls() { return true; }
void UpdateIOSTouchControls(double interval) { ++touchUpdates; }
void ResetIOSTouchControls() { ++touchResets; }

@implementation UIScreen
- (CADisplayLink*)displayLinkWithTarget:(id)target selector:(SEL)selector
{
    if (failRegistration)
        return nil;
    testDisplayLink = [CADisplayLink new];
    testDisplayLink.target = target;
    testDisplayLink.targetTimestamp = 1.0 / 120;
    testDisplayLink.selector = selector;
    return testDisplayLink;
}
@end

@implementation UIWindow
@end

@implementation CADisplayLink
- (void)invalidate
{
    self.paused = YES;
}
- (void)addToRunLoop:(NSRunLoop*)runLoop forMode:(NSRunLoopMode)mode
{
    self.runLoop = runLoop;
    self.mode = mode;
}
- (void)fire
{
    if (!self.paused)
    {
        using Callback = void(*)(id, SEL, CADisplayLink*);
        reinterpret_cast<Callback>(objc_msgSend)(self.target, self.selector, self);
    }
}
@end

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
    assert(!StartIOSEventLoop());
    testWindow = [UIWindow new];
    testWindow.screen = [UIScreen new];
    testWindow.screen.maximumFramesPerSecond = 120;
    GameWindow::s_renderWindow.window = (__bridge void*)testWindow;
    failRegistration = true;
    assert(!StartIOSEventLoop());
    assert(!testDisplayLink);
    failRegistration = false;
    UIApplication.sharedApplication.applicationState = UIApplicationStateInactive;
    assert(StartIOSEventLoop());
    assert(testDisplayLink && testDisplayLink.paused);
    assert(testDisplayLink.runLoop == NSRunLoop.mainRunLoop);
    assert([testDisplayLink.mode isEqualToString:NSRunLoopCommonModes]);
    assert(testDisplayLink.preferredFrameRateRange.preferred == 120);
    assert(testDisplayLink.preferredFrameRateRange.minimum == 120);
    assert(testDisplayLink.preferredFrameRateRange.maximum == 120);
    assert(!hid::IsInputAllowed());
    assert(!audioActive && !metalActive && !app_lifecycle::GetExecutionGate().IsActive());
    UIApplication.sharedApplication.applicationState = UIApplicationStateActive;
    [NSNotificationCenter.defaultCenter postNotificationName:UIApplicationDidBecomeActiveNotification object:nil];
    assert(!testDisplayLink.paused);
    assert(hid::IsInputAllowed());
    assert(audioActive && metalActive && app_lifecycle::GetExecutionGate().IsActive());

    // A config callback can arrive on the guest thread. UIKit changes on a tick.
    std::async(std::launch::async, [] { SetIOSFrameRateLimit(60); }).get();
    assert(testDisplayLink.preferredFrameRateRange.preferred == 120);
    [testDisplayLink fire];
    assert(testDisplayLink.preferredFrameRateRange.preferred == 60);
    assert(testDisplayLink.preferredFrameRateRange.maximum == 120);
    SetIOSFrameRateLimit(90);
    [testDisplayLink fire];
    assert(testDisplayLink.preferredFrameRateRange.minimum == 90);
    assert(testDisplayLink.preferredFrameRateRange.maximum == 120);
    // Unlimited and above-display caps use the hardware maximum, including 60 Hz phones.
    for (int limit : {FPS_MAX, 240, 0})
    {
        SetIOSFrameRateLimit(limit);
        [testDisplayLink fire];
        assert(testDisplayLink.preferredFrameRateRange.preferred == 120);
        testWindow.screen.maximumFramesPerSecond = 60;
        [testDisplayLink fire];
        assert(testDisplayLink.preferredFrameRateRange.preferred == 60);
        assert(testDisplayLink.preferredFrameRateRange.maximum == 60);
        testWindow.screen.maximumFramesPerSecond = 120;
    }
    SetIOSFrameRateLimit(120);
    [testDisplayLink fire];
}

void TestIOSLoopTick()
{
    const unsigned previousUpdates = GameWindow::updates;
    const unsigned previousTouchUpdates = touchUpdates;
    [testDisplayLink fire];
    assert(GameWindow::updates == previousUpdates + 1);
    assert(touchUpdates == previousTouchUpdates + 1);
}

void TestIOSFocusLifecycle()
{
    using namespace std::chrono_literals;
    UIApplication.sharedApplication.applicationState = UIApplicationStateBackground;
    const unsigned previousTouchResets = touchResets;
    [NSNotificationCenter.defaultCenter postNotificationName:UIApplicationWillResignActiveNotification object:nil];
    assert(touchResets == previousTouchResets + 1);
    assert(testDisplayLink.paused);
    const unsigned pausedUpdates = GameWindow::updates;
    [testDisplayLink fire];
    assert(GameWindow::updates == pausedUpdates);
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
    SetIOSFrameRateLimit(30);
    UIApplication.sharedApplication.applicationState = UIApplicationStateInactive;
    [NSNotificationCenter.defaultCenter postNotificationName:UIApplicationWillEnterForegroundNotification object:nil];
    assert(!audioActive && !metalActive && !app_lifecycle::GetExecutionGate().IsActive());
    assert(worker.wait_for(30ms) == std::future_status::timeout);
    UIApplication.sharedApplication.applicationState = UIApplicationStateActive;
    [NSNotificationCenter.defaultCenter postNotificationName:UIApplicationDidBecomeActiveNotification object:nil];
    assert(!testDisplayLink.paused);
    assert(testDisplayLink.preferredFrameRateRange.preferred == 30);
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
