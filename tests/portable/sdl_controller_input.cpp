#include <stdafx.h>
#include <hid/hid.h>
#include <kernel/xdm.h>
#include <user/config.h>
#include <ui/game_window.h>

#ifdef INPUT_TEST_IOS_LOOP
void TestIOSLoopInit();
void TestIOSLoopTick();
void TestIOSFocusLifecycle();
#endif

static const auto mainThread = std::this_thread::get_id();
static unsigned ledUpdates;

static int SDLCALL SetLED(void*, Uint8, Uint8, Uint8)
{
    assert(std::this_thread::get_id() == mainThread);
    ++ledUpdates;
    return 0;
}

static int AttachController()
{
    SDL_VirtualJoystickDesc description{};
    description.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    description.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    description.naxes = SDL_CONTROLLER_AXIS_MAX;
    description.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    description.SetLED = SetLED;
    return SDL_JoystickAttachVirtualEx(&description);
}

static void PumpEvents()
{
#ifdef INPUT_TEST_IOS_LOOP
    TestIOSLoopTick();
#else
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    hid::Update();
#endif
}

int main()
{
    // Keep local physical controllers out of this virtual-device test.
    SDL_SetHint(SDL_HINT_JOYSTICK_MFI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_IOKIT, "0");
    hid::Init();
    assert(SDL_WasInit(SDL_INIT_GAMECONTROLLER));
#ifdef INPUT_TEST_IOS_LOOP
    TestIOSLoopInit();
#endif
    assert(hid::GetState(0, nullptr) == ERROR_BAD_ARGUMENTS);
    XAMINPUT_STATE state{};
    assert(hid::GetState(0, &state) == ERROR_DEVICE_NOT_CONNECTED);
    assert(!hid::HasConnectedController());
    XAMINPUT_GAMEPAD touch{};
    touch.wButtons = XAMINPUT_GAMEPAD_START | XAMINPUT_GAMEPAD_A;
    touch.sThumbLX = 19000;
    touch.sThumbRY = -12000;
    hid::SetTouchControllerState(true, touch);
    assert(hid::IsTouchControllerActive());
    assert(hid::GetState(0, &state) == ERROR_SUCCESS);
    assert(state.Gamepad.wButtons == touch.wButtons && state.Gamepad.sThumbLX == touch.sThumbLX);
    assert(state.Gamepad.sThumbRY == touch.sThumbRY);
    assert(hid::g_inputDevice == hid::EInputDevice::Xbox);
    assert(hid::GetState(1, &state) == ERROR_DEVICE_NOT_CONNECTED);
    XAMINPUT_CAPABILITIES touchCaps{};
    assert(hid::GetCapabilities(0, &touchCaps) == ERROR_SUCCESS);
    assert(touchCaps.Type == XAMINPUT_DEVTYPE_GAMEPAD);
    assert((touchCaps.Gamepad.wButtons & (XAMINPUT_GAMEPAD_BACK | XAMINPUT_GAMEPAD_X | XAMINPUT_GAMEPAD_B)) != 0);
    XAMINPUT_VIBRATION noRumble{};
    assert(hid::SetState(0, &noRumble) == ERROR_SUCCESS);
    hid::SetTouchControllerState(false, touch);
    assert(hid::GetState(0, &state) == ERROR_DEVICE_NOT_CONNECTED);
    assert(state.Gamepad.wButtons == 0 && state.Gamepad.sThumbLX == 0);

    const int index = AttachController();
    assert(index >= 0);
    SDL_Joystick* joystick = SDL_JoystickOpen(index);
    assert(joystick);
    PumpEvents();
    assert(hid::HasConnectedController());
    assert(!hid::IsTouchControllerActive());
    assert(!SDL_HasEvents(SDL_FIRSTEVENT, SDL_LASTEVENT));
    // A connected, idle pad must be visible before the first input event.
    assert(hid::GetState(0, &state) == ERROR_SUCCESS);
    assert(state.Gamepad.wButtons == 0);
    // A connected, idle hardware pad takes priority over any stale touch snapshot.
    hid::SetTouchControllerState(true, touch);
    assert(hid::GetState(0, &state) == ERROR_SUCCESS);
    assert(state.Gamepad.wButtons == 0 && state.Gamepad.sThumbLX == 0);
    hid::SetTouchControllerState(false, {});

    assert(SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_START, SDL_PRESSED) == 0);
    assert(SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_A, SDL_PRESSED) == 0);
    // The guest cannot see new device input until UIKit's callback pumps SDL.
    assert(hid::GetState(0, &state) == ERROR_SUCCESS);
    assert(state.Gamepad.wButtons == 0);
    PumpEvents();
    assert(hid::GetState(0, &state) == ERROR_SUCCESS);
    assert(state.Gamepad.wButtons == (XAMINPUT_GAMEPAD_START | XAMINPUT_GAMEPAD_A));
    assert(SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_START, SDL_RELEASED) == 0);
    assert(SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_A, SDL_RELEASED) == 0);
    assert(SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_PRESSED) == 0);
    assert(SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, 16000) == 0);
    PumpEvents();
    assert(hid::GetState(0, &state) == ERROR_SUCCESS);
    assert(state.Gamepad.wButtons == XAMINPUT_GAMEPAD_DPAD_DOWN);
    assert(state.Gamepad.sThumbLX == 16000);

#ifdef INPUT_TEST_IOS_LOOP
    TestIOSFocusLifecycle();
#endif
    GameWindow::s_isFocused = false;
    assert(!hid::IsInputAllowed());
    Config::AllowBackgroundInput = true;
    assert(hid::IsInputAllowed());
    Config::AllowBackgroundInput = false;
    GameWindow::s_isFocused = true;
    assert(hid::IsInputAllowed());

    const unsigned previousLEDUpdates = ledUpdates;
    SDL_User_EvilSonic(true);
    assert(ledUpdates == previousLEDUpdates);
    PumpEvents();
    assert(ledUpdates == previousLEDUpdates + 1);

    // Exercise the iOS threading arrangement: SDL updates on the main thread,
    // while the guest continuously reads states, capabilities, and rumble.
    std::atomic<bool> stop = false;
    std::atomic<unsigned> reads = 0;
    std::thread guest([&] {
        while (!stop)
        {
            XAMINPUT_STATE snapshot{};
            const auto result = hid::GetState(0, &snapshot);
            assert(result == ERROR_SUCCESS || result == ERROR_DEVICE_NOT_CONNECTED);
            if (result == ERROR_SUCCESS)
                assert(snapshot.Gamepad.wButtons == 0 || snapshot.Gamepad.wButtons == XAMINPUT_GAMEPAD_DPAD_DOWN);
            XAMINPUT_CAPABILITIES capabilities{};
            hid::GetCapabilities(0, &capabilities);
            XAMINPUT_VIBRATION vibration{};
            hid::SetState(0, &vibration);
            SDL_User_EvilSonic((reads % 2) != 0);
            ++reads;
        }
    });
    while (reads == 0)
        std::this_thread::yield();
    for (int frame = 0; frame < 500; ++frame)
    {
        assert(SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_DPAD_DOWN, frame % 2) == 0);
        PumpEvents();
    }
    assert(SDL_JoystickDetachVirtual(index) == 0);
    PumpEvents();
    assert(hid::GetState(0, &state) == ERROR_DEVICE_NOT_CONNECTED);
    assert(state.Gamepad.wButtons == 0);
    assert(state.Gamepad.sThumbLX == 0);
    assert(!hid::HasConnectedController());
    stop = true;
    guest.join();
    SDL_JoystickClose(joystick);

    const int replacement = AttachController();
    assert(replacement >= 0);
    PumpEvents();
    assert(hid::GetState(0, &state) == ERROR_SUCCESS);
    assert(state.Gamepad.wButtons == 0);
    assert(state.Gamepad.sThumbLX == 0);
    assert(SDL_JoystickDetachVirtual(replacement) == 0);
    PumpEvents();
    assert(!hid::HasConnectedController());
    const int firstPad = AttachController();
    const int secondPad = AttachController();
    assert(firstPad == 0 && secondPad == 1);
    PumpEvents();
    assert(SDL_JoystickDetachVirtual(firstPad) == 0);
    PumpEvents();
    assert(hid::HasConnectedController());
    assert(hid::GetState(0, &state) == ERROR_SUCCESS); // The other connected pad becomes active.
    assert(!hid::IsTouchControllerActive());
    assert(SDL_JoystickDetachVirtual(0) == 0);
    PumpEvents();
    assert(!hid::HasConnectedController());
    // Touch input returns after unplugging, with no carried-over movement.
    hid::SetTouchControllerState(true, {});
    assert(hid::GetState(0, &state) == ERROR_SUCCESS);
    assert(state.Gamepad.wButtons == 0 && state.Gamepad.sThumbLX == 0);
    std::atomic<bool> touchDone = false;
    std::thread touchReader([&] {
        while (!touchDone)
        {
            XAMINPUT_STATE snapshot{};
            assert(hid::GetState(0, &snapshot) == ERROR_SUCCESS);
            assert((snapshot.Gamepad.wButtons == 0 && snapshot.Gamepad.sThumbLX == 0) ||
                (snapshot.Gamepad.wButtons == touch.wButtons && snapshot.Gamepad.sThumbLX == touch.sThumbLX));
        }
    });
    for (int i = 0; i < 2000; ++i)
        hid::SetTouchControllerState(true, i % 2 ? touch : XAMINPUT_GAMEPAD{});
    touchDone = true;
    touchReader.join();
    hid::SetTouchControllerState(false, {});
    SDL_Quit();
}
