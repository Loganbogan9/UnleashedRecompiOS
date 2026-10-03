#pragma once

namespace hid
{
    enum class EInputDevice
    {
        Unknown,
        Keyboard,
        Mouse,
        Xbox,
        PlayStation
    };

    enum class EInputDeviceExplicit
    {
        Unknown,
        Xbox360,
        XboxOne,
        DualShock3,
        DualShock4,
        SwitchPro,
        Virtual,
        DualSense,
        Luna,
        Stadia,
        NvShield,
        SwitchJCLeft,
        SwitchJCRight,
        SwitchJCPair
    };

    extern EInputDevice g_inputDevice;
    extern EInputDevice g_inputDeviceController;
    extern EInputDeviceExplicit g_inputDeviceExplicit;

    extern uint16_t g_prohibitedButtons;
    extern bool g_isLeftStickProhibited;
    extern bool g_isRightStickProhibited;

    void Init();
    void Update();

    uint32_t GetState(uint32_t dwUserIndex, XAMINPUT_STATE* pState);
    uint32_t SetState(uint32_t dwUserIndex, XAMINPUT_VIBRATION* pVibration);
    uint32_t GetCapabilities(uint32_t dwUserIndex, XAMINPUT_CAPABILITIES* pCaps);

    bool HasConnectedController();
#if defined(UNLEASHED_RECOMP_IOS)
    // UIKit publishes a complete snapshot; guest readers share the SDL joystick lock.
    void SetTouchControllerState(bool enabled, const XAMINPUT_GAMEPAD& state);
    bool IsTouchControllerActive();
#endif

    void SetProhibitedInputs(uint16_t wButtons = 0, bool leftStick = false, bool rightStick = false);
    bool IsInputAllowed();
    bool IsInputDeviceController();
}
