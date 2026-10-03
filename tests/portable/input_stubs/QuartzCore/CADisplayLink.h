#pragma once
#import <Foundation/Foundation.h>

struct CAFrameRateRange { float minimum, maximum, preferred; };
inline CAFrameRateRange CAFrameRateRangeMake(float minimum, float maximum, float preferred)
{
    return {minimum, maximum, preferred};
}
inline bool CAFrameRateRangeIsEqualToRange(CAFrameRateRange range, CAFrameRateRange other)
{
    return range.minimum == other.minimum && range.maximum == other.maximum && range.preferred == other.preferred;
}

@interface CADisplayLink : NSObject
@property BOOL paused;
@property CAFrameRateRange preferredFrameRateRange;
@property double timestamp;
@property double targetTimestamp;
@property(strong) id target;
@property SEL selector;
@property(strong) NSRunLoop* runLoop;
@property(copy) NSRunLoopMode mode;
- (void)addToRunLoop:(NSRunLoop*)runLoop forMode:(NSRunLoopMode)mode;
- (void)fire;
- (void)invalidate;
@end
