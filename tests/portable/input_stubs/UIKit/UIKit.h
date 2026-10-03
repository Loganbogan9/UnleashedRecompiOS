#pragma once
#import <Foundation/Foundation.h>

typedef NS_ENUM(NSInteger, UIApplicationState) {
    UIApplicationStateActive,
    UIApplicationStateInactive,
    UIApplicationStateBackground
};

@interface UIApplication : NSObject
@property(class, readonly) UIApplication* sharedApplication;
@property UIApplicationState applicationState;
@end

extern NSNotificationName const UIApplicationWillResignActiveNotification;
extern NSNotificationName const UIApplicationDidBecomeActiveNotification;
extern NSNotificationName const UIApplicationDidEnterBackgroundNotification;
extern NSNotificationName const UIApplicationWillEnterForegroundNotification;
