#pragma once

// UIKit main thread only; install after the installer has finished.
bool StartIOSTouchControls();
void UpdateIOSTouchControls(double frameInterval);
void ResetIOSTouchControls();

// May be called from the guest/settings thread; applied on the next UIKit tick.
void SetIOSTouchCameraSensitivity(float sensitivity);
