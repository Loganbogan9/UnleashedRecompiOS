#include <stdafx.h>
#include "ios_touch_controls.h"
#include <hid/hid.h>
#include <hid/touch_gamepad.h>
#include <ui/game_window.h>
#include <user/config.h>
#include <atomic>
#import <UIKit/UIKit.h>

static std::atomic<float> g_cameraSensitivity{hid::TouchGamepad::DefaultCameraSensitivity};

void SetIOSTouchCameraSensitivity(float sensitivity)
{
    g_cameraSensitivity.store(hid::TouchGamepad::ClampCameraSensitivity(sensitivity), std::memory_order_relaxed);
}

static hid::TouchPoint TouchPosition(UITouch* touch, UIView* view)
{
    const CGPoint point = [touch locationInView:view];
    return {(float)point.x, (float)point.y};
}

@interface UnleashedTouchControlsView : UIView
{
    hid::TouchGamepad _gamepad;
}
- (void)updateControls:(double)frameInterval enabled:(BOOL)enabled;
- (void)resetControls;
@end

@implementation UnleashedTouchControlsView
- (instancetype)initWithFrame:(CGRect)frame
{
    if ((self = [super initWithFrame:frame]))
    {
        self.opaque = NO;
        self.backgroundColor = UIColor.clearColor;
        self.multipleTouchEnabled = YES;
        self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        self.hidden = YES;
    }
    return self;
}
- (void)publishState
{
    hid::SetTouchControllerState(_gamepad.IsEnabled(), _gamepad.State());
}
- (void)layoutSubviews
{
    [super layoutSubviews];
    const UIEdgeInsets insets = self.safeAreaInsets;
    _gamepad.SetBounds(self.bounds.size.width, self.bounds.size.height, insets.left, insets.top, insets.right, insets.bottom);
    [self publishState];
    [self setNeedsDisplay];
}
- (void)updateControls:(double)frameInterval enabled:(BOOL)enabled
{
    _gamepad.SetCameraSensitivity(g_cameraSensitivity.load(std::memory_order_relaxed));
    if (_gamepad.IsEnabled() != (bool)enabled)
    {
        _gamepad.SetEnabled(enabled);
        self.hidden = !enabled;
        [self setNeedsDisplay];
    }
    _gamepad.Tick(frameInterval);
    [self publishState];
}
- (void)resetControls
{
    _gamepad.SetEnabled(false);
    self.hidden = YES;
    [self publishState];
    [self setNeedsDisplay];
}
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    for (UITouch* touch in touches)
        _gamepad.Begin((uintptr_t)(__bridge void*)touch, TouchPosition(touch, self));
    [self publishState];
    [self setNeedsDisplay];
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    for (UITouch* touch in touches)
        _gamepad.Move((uintptr_t)(__bridge void*)touch, TouchPosition(touch, self));
    [self publishState];
    [self setNeedsDisplay];
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    for (UITouch* touch in touches)
        _gamepad.End((uintptr_t)(__bridge void*)touch);
    [self publishState];
    [self setNeedsDisplay];
}
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    [self touchesEnded:touches withEvent:event];
}
- (void)drawRect:(CGRect)rect
{
    const auto& state = _gamepad.State();
    const auto& stick = _gamepad.Stick();
    const float scale = _gamepad.Scale();
    auto drawArea = [scale](const hid::TouchArea& area, bool held) {
        const CGRect bounds = CGRectMake(area.center.x - area.width / 2, area.center.y - area.height / 2, area.width, area.height);
        UIBezierPath* path = area.circular ? [UIBezierPath bezierPathWithOvalInRect:bounds]
            : [UIBezierPath bezierPathWithRoundedRect:bounds cornerRadius:area.height / 2];
        [[UIColor colorWithWhite:held ? 0.7 : 0.08 alpha:held ? 0.5 : 0.35] setFill];
        [[UIColor colorWithWhite:1 alpha:0.6] setStroke];
        path.lineWidth = 1.5 * scale;
        [path fill];
        [path stroke];
    };
    drawArea(stick, false);
    const float travel = _gamepad.StickTravel();
    const hid::TouchArea knob{{stick.center.x + state.sThumbLX / 32767.0f * travel,
        stick.center.y - state.sThumbLY / 32767.0f * travel}, 42 * scale, 42 * scale, true};
    drawArea(knob, state.sThumbLX || state.sThumbLY);
    for (const auto& button : _gamepad.Buttons())
    {
        drawArea(button.area, (state.wButtons & button.mask) != 0);
        NSString* title = [NSString stringWithUTF8String:button.label];
        NSDictionary* attributes = @{NSFontAttributeName: [UIFont systemFontOfSize:(button.area.circular ? 22 : 14) * scale weight:UIFontWeightSemibold],
            NSForegroundColorAttributeName: UIColor.whiteColor};
        const CGSize size = [title sizeWithAttributes:attributes];
        [title drawAtPoint:CGPointMake(button.area.center.x - size.width / 2, button.area.center.y - size.height / 2) withAttributes:attributes];
    }
}
@end

static UnleashedTouchControlsView* g_touchControls;

bool StartIOSTouchControls()
{
    assert(NSThread.isMainThread);
    if (g_touchControls != nil)
        return true;
    UIWindow* window = (__bridge UIWindow*)GameWindow::s_renderWindow.window;
    UIView* parent = window.rootViewController.view;
    if (parent == nil)
        return false;
    SetIOSTouchCameraSensitivity(Config::TouchCameraSensitivity);
    g_touchControls = [[UnleashedTouchControlsView alloc] initWithFrame:parent.bounds];
    [parent addSubview:g_touchControls];
    [g_touchControls setNeedsLayout];
    [g_touchControls layoutIfNeeded];
    return true;
}

void UpdateIOSTouchControls(double frameInterval)
{
    assert(NSThread.isMainThread);
    UIWindow* window = (__bridge UIWindow*)GameWindow::s_renderWindow.window;
    const bool enabled = UIApplication.sharedApplication.applicationState == UIApplicationStateActive
        && window.rootViewController.presentedViewController == nil && !hid::HasConnectedController();
    [g_touchControls updateControls:frameInterval enabled:enabled];
}

void ResetIOSTouchControls()
{
    assert(NSThread.isMainThread);
    [g_touchControls resetControls];
}
