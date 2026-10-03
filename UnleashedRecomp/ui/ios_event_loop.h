#pragma once

// Call on UIKit's main thread before starting the guest and returning from main.
bool StartIOSEventLoop();

// May be called from the guest/config thread; UIKit applies it on the next tick.
void SetIOSFrameRateLimit(int frameRate);
