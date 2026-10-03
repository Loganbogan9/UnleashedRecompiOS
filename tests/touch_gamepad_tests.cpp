#include <hid/touch_gamepad.h>
#include <cassert>
#include <limits>

static const hid::TouchButton& Button(const hid::TouchGamepad& pad, uint16_t mask)
{
    for (const auto& button : pad.Buttons())
        if (button.mask == mask)
            return button;
    assert(false);
    return pad.Buttons()[0];
}

static XAMINPUT_GAMEPAD CameraDrag(hid::TouchGamepad& pad, float distance, double interval)
{
    pad.Clear();
    pad.SetEnabled(true);
    pad.Begin(1, {300, 100});
    pad.Move(1, {300 + distance, 100 + distance / 2});
    pad.Tick(interval);
    return pad.State();
}

static void TestCameraSensitivity()
{
    hid::TouchGamepad pad;
    pad.SetBounds(852, 393, 59, 0, 59, 21);
    const auto normal = CameraDrag(pad, 1, 1.0 / 60);
    pad.SetCameraSensitivity(0.25f);
    const auto slow = CameraDrag(pad, 1, 1.0 / 60);
    pad.SetCameraSensitivity(3.0f);
    const auto fast = CameraDrag(pad, 1, 1.0 / 60);
    assert(slow.sThumbRX > 0 && slow.sThumbRY < 0);
    assert(slow.sThumbRX < normal.sThumbRX && normal.sThumbRX < fast.sThumbRX);
    assert(slow.sThumbRY > normal.sThumbRY && normal.sThumbRY > fast.sThumbRY);

    // Scaling remains independent of display refresh and does not create motion.
    for (float sensitivity : {0.25f, 1.0f, 3.0f})
    {
        pad.SetCameraSensitivity(sensitivity);
        const auto sixty = CameraDrag(pad, 1, 1.0 / 60);
        const auto oneTwenty = CameraDrag(pad, 0.5f, 1.0 / 120);
        assert(std::abs(sixty.sThumbRX - oneTwenty.sThumbRX) <= 1);
        assert(std::abs(sixty.sThumbRY - oneTwenty.sThumbRY) <= 1);
        pad.Tick(1.0 / 120);
        assert(pad.State().sThumbRX == 0 && pad.State().sThumbRY == 0);
    }

    // Clamp edited config values, including non-finite TOML floats, to a usable range.
    pad.SetCameraSensitivity(-1.0f);
    assert(CameraDrag(pad, 1, 1.0 / 60).sThumbRX == slow.sThumbRX);
    pad.SetCameraSensitivity(100.0f);
    assert(CameraDrag(pad, 1, 1.0 / 60).sThumbRX == fast.sThumbRX);
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
    {
        pad.SetCameraSensitivity(invalid);
        assert(CameraDrag(pad, 1, 1.0 / 60).sThumbRX == normal.sThumbRX);
    }
    pad.SetCameraSensitivity(3.0f);
    const auto saturated = CameraDrag(pad, 1000, 1.0 / 120);
    assert(std::hypot(saturated.sThumbRX, saturated.sThumbRY) <= 32768);

    // Handoff and rotation clear fingers while preserving the chosen sensitivity.
    pad.SetEnabled(false);
    pad.SetEnabled(true);
    pad.SetBounds(568, 320, 0, 0, 0, 0);
    pad.SetBounds(852, 393, 59, 0, 59, 21);
    assert(CameraDrag(pad, 1, 1.0 / 60).sThumbRX == fast.sThumbRX);

    // A live change affects the next drag without releasing movement or action buttons.
    pad.Clear();
    const auto stick = pad.Stick().center;
    pad.Begin(0, stick);
    pad.Move(0, {stick.x + 10, stick.y});
    pad.Begin(2, Button(pad, XAMINPUT_GAMEPAD_A).area.center);
    pad.Begin(1, {300, 100});
    const auto left = pad.State().sThumbLX;
    pad.SetCameraSensitivity(0.25f);
    pad.Move(1, {301, 100.5f});
    pad.Tick(1.0 / 60);
    assert(pad.State().sThumbRX == slow.sThumbRX);
    pad.SetCameraSensitivity(3.0f);
    pad.Move(1, {302, 101});
    pad.Tick(1.0 / 60);
    assert(pad.State().sThumbRX == fast.sThumbRX);
    assert(pad.State().sThumbLX == left && pad.State().wButtons == XAMINPUT_GAMEPAD_A);
}

int main()
{
    TestCameraSensitivity();
    hid::TouchGamepad pad;
    pad.SetBounds(852, 393, 59, 0, 59, 21);
    pad.Begin(0, Button(pad, XAMINPUT_GAMEPAD_A).area.center);
    assert(pad.State().wButtons == 0); // Hidden controls ignore input.
    pad.SetEnabled(true);
    const auto stick = pad.Stick().center;
    pad.Begin(0, stick);
    pad.Move(0, {stick.x + 1000, stick.y - 1000});
    assert(pad.State().sThumbLX > 23000 && pad.State().sThumbLY > 23000);
    assert(std::hypot(pad.State().sThumbLX, pad.State().sThumbLY) <= 32768);
    uint64_t finger = 1;
    uint16_t allButtons = 0;
    for (const auto& button : pad.Buttons())
    {
        pad.Begin(finger++, button.area.center);
        allButtons |= button.mask;
        assert((pad.State().wButtons & allButtons) == allButtons);
    }
    // Buttons, movement and a camera drag can all be held independently.
    const hid::TouchPoint center{426, 180};
    pad.Begin(6, center);
    pad.Move(6, {center.x + 4, center.y + 2});
    pad.Tick(1.0 / 120);
    assert(pad.State().sThumbRX > 0 && pad.State().sThumbRY < 0);
    assert(pad.State().wButtons == allButtons && pad.State().sThumbLX > 0);
    pad.Tick(1.0 / 120);
    assert(pad.State().sThumbRX == 0 && pad.State().sThumbRY == 0); // Holding still stops the camera.
    pad.Move(6, {center.x + 6, center.y + 2});
    pad.End(6);
    pad.Tick(1.0 / 60);
    assert(pad.State().sThumbRX == 0);
    pad.End(0);
    assert(pad.State().sThumbLX == 0 && pad.State().sThumbLY == 0);
    assert(pad.State().wButtons == allButtons);
    pad.Clear();

    const auto a = Button(pad, XAMINPUT_GAMEPAD_A).area;
    pad.Begin(1, a.center);
    pad.Begin(2, a.center);
    pad.End(1);
    assert(pad.State().wButtons == XAMINPUT_GAMEPAD_A); // Another finger still holds A.
    pad.Move(2, {a.center.x - 200, a.center.y});
    assert(pad.State().wButtons == 0);
    pad.Move(2, a.center);
    assert(pad.State().wButtons == XAMINPUT_GAMEPAD_A);
    pad.SetEnabled(false);
    assert(pad.State().wButtons == 0 && pad.State().sThumbLX == 0);
    pad.SetEnabled(true);
    pad.Move(2, a.center); // A finger held across controller handoff cannot reactivate.
    assert(pad.State().wButtons == 0);

    // The same drag speed produces the same camera input at 60 and 120 Hz.
    pad.Begin(1, center);
    pad.Move(1, {center.x + 2, center.y});
    pad.Tick(1.0 / 120);
    const auto fastCamera = pad.State().sThumbRX;
    pad.Move(1, {center.x + 6, center.y});
    pad.Tick(1.0 / 60);
    assert(std::abs(pad.State().sThumbRX - fastCamera) <= 1);
    pad.Clear();

    // Keep every drawn control inside the safe area on small phones and large iPads.
    for (auto bounds : {std::array<float, 6>{568, 320, 0, 0, 0, 0},
        std::array<float, 6>{852, 393, 59, 0, 59, 21},
        std::array<float, 6>{1194, 834, 0, 0, 0, 20}})
    {
        pad.Begin(1, Button(pad, XAMINPUT_GAMEPAD_A).area.center);
        pad.SetBounds(bounds[0], bounds[1], bounds[2], bounds[3], bounds[4], bounds[5]);
        assert(pad.State().wButtons == 0);
        auto inside = [&](const hid::TouchArea& area) {
            assert(area.center.x - area.width / 2 >= bounds[2]);
            assert(area.center.y - area.height / 2 >= bounds[3]);
            assert(area.center.x + area.width / 2 <= bounds[0] - bounds[4]);
            assert(area.center.y + area.height / 2 <= bounds[1] - bounds[5]);
        };
        inside(pad.Stick());
        for (const auto& button : pad.Buttons())
            inside(button.area);
    }
    pad.Clear();
    // Unknown/repeated releases and contact overflow remain neutral after cancellation.
    for (int i = 0; i < 20; ++i)
        pad.Begin(i, {600, 300});
    pad.Clear();
    for (int i = 0; i < 20; ++i)
        pad.End(i);
    pad.Tick(1.0 / 120);
    assert(pad.State().wButtons == 0 && pad.State().sThumbRX == 0);
}
