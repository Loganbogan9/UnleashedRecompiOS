#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <xbox.h>

namespace hid
{
    struct TouchPoint { float x, y; };
    struct TouchArea
    {
        TouchPoint center;
        float width, height;
        bool circular;

        bool Contains(TouchPoint point) const
        {
            const float x = (point.x - center.x) / (width / 2);
            const float y = (point.y - center.y) / (height / 2);
            return circular ? x * x + y * y <= 1 : std::abs(x) <= 1 && std::abs(y) <= 1;
        }
    };
    struct TouchButton { TouchArea area; uint16_t mask; const char* label; };

    // Owned by UIKit's main thread. Only its gamepad snapshot crosses to the guest.
    class TouchGamepad
    {
        enum class Role { None, LeftStick, Camera, Button, Ignored };
        struct Contact
        {
            uint64_t id{};
            Role role = Role::None;
            TouchPoint point{};
            size_t button{};
        };
        std::array<Contact, 16> contacts{};
        std::array<TouchButton, 7> buttons{};
        TouchArea stick{};
        float scale = 1, stickTravel = 44;
        float cameraSensitivity = DefaultCameraSensitivity;
        std::array<float, 6> bounds{};
        TouchPoint cameraDelta{};
        XAMINPUT_GAMEPAD state{};
        bool enabled = false;

        Contact* Find(uint64_t id)
        {
            for (auto& contact : contacts)
                if (contact.role != Role::None && contact.id == id)
                    return &contact;
            return nullptr;
        }
        bool HasRole(Role role) const
        {
            return std::any_of(contacts.begin(), contacts.end(), [role](const auto& contact) { return contact.role == role; });
        }
        static int16_t Axis(float value)
        {
            return (int16_t)std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f);
        }
        void UpdateHeldInputs()
        {
            state.wButtons = 0;
            state.sThumbLX = state.sThumbLY = 0;
            for (const auto& contact : contacts)
            {
                if (contact.role == Role::Button && buttons[contact.button].area.Contains(contact.point))
                    state.wButtons |= buttons[contact.button].mask;
                else if (contact.role == Role::LeftStick)
                {
                    float x = (contact.point.x - stick.center.x) / stickTravel;
                    float y = (stick.center.y - contact.point.y) / stickTravel;
                    const float length = std::max(1.0f, std::hypot(x, y));
                    state.sThumbLX = Axis(x / length);
                    state.sThumbLY = Axis(y / length);
                }
            }
        }

    public:
        static constexpr float MinCameraSensitivity = 0.25f;
        static constexpr float DefaultCameraSensitivity = 1.0f;
        static constexpr float MaxCameraSensitivity = 3.0f;

        static float ClampCameraSensitivity(float value)
        {
            return std::isfinite(value) ? std::clamp(value, MinCameraSensitivity, MaxCameraSensitivity) : DefaultCameraSensitivity;
        }
        void SetCameraSensitivity(float value)
        {
            cameraSensitivity = ClampCameraSensitivity(value);
        }

        const auto& Buttons() const { return buttons; }
        const TouchArea& Stick() const { return stick; }
        float Scale() const { return scale; }
        float StickTravel() const { return stickTravel; }
        const XAMINPUT_GAMEPAD& State() const { return state; }
        bool IsEnabled() const { return enabled; }

        void Clear()
        {
            contacts = {};
            cameraDelta = {};
            state = {};
        }
        void SetEnabled(bool value)
        {
            if (enabled != value)
                Clear();
            enabled = value;
        }
        void SetBounds(float width, float height, float left, float top, float right, float bottom)
        {
            const std::array<float, 6> next{width, height, left, top, right, bottom};
            if (bounds == next)
                return;
            bounds = next;
            Clear(); // Rotation or a safe-area change invalidates the old touch positions.
            const float safeWidth = std::max(1.0f, width - left - right);
            const float safeHeight = std::max(1.0f, height - top - bottom);
            scale = std::clamp(std::min(safeWidth / 640, safeHeight / 360), 0.75f, 1.4f);
            const float maxX = width - right, maxY = height - bottom;
            const float middleX = left + safeWidth / 2;
            stick = {{left + 76 * scale, maxY - 90 * scale}, 116 * scale, 116 * scale, true};
            stickTravel = 44 * scale;
            buttons = {{
                {{{middleX - 42 * scale, top + 28 * scale}, 68 * scale, 36 * scale, false}, XAMINPUT_GAMEPAD_BACK, "Back"},
                {{{middleX + 42 * scale, top + 28 * scale}, 68 * scale, 36 * scale, false}, XAMINPUT_GAMEPAD_START, "Start"},
                {{{left + 76 * scale, maxY - 214 * scale}, 84 * scale, 44 * scale, false}, XAMINPUT_GAMEPAD_LEFT_SHOULDER, "LB"},
                {{{maxX - 80 * scale, maxY - 214 * scale}, 84 * scale, 44 * scale, false}, XAMINPUT_GAMEPAD_RIGHT_SHOULDER, "RB"},
                {{{maxX - 118 * scale, maxY - 150 * scale}, 60 * scale, 60 * scale, true}, XAMINPUT_GAMEPAD_X, "X"},
                {{{maxX - 80 * scale, maxY - 72 * scale}, 60 * scale, 60 * scale, true}, XAMINPUT_GAMEPAD_A, "A"},
                {{{maxX - 42 * scale, maxY - 150 * scale}, 60 * scale, 60 * scale, true}, XAMINPUT_GAMEPAD_B, "B"}
            }};
        }
        void Begin(uint64_t id, TouchPoint point)
        {
            if (!enabled || Find(id))
                return;
            auto free = std::find_if(contacts.begin(), contacts.end(), [](const auto& contact) { return contact.role == Role::None; });
            if (free == contacts.end())
                return;
            *free = {id, Role::Ignored, point, 0};
            for (size_t i = 0; i < buttons.size(); ++i)
            {
                if (buttons[i].area.Contains(point))
                {
                    free->role = Role::Button;
                    free->button = i;
                    UpdateHeldInputs();
                    return;
                }
            }
            if (stick.Contains(point))
                free->role = HasRole(Role::LeftStick) ? Role::Ignored : Role::LeftStick;
            else if (!HasRole(Role::Camera))
                free->role = Role::Camera;
            UpdateHeldInputs();
        }
        void Move(uint64_t id, TouchPoint point)
        {
            auto* contact = Find(id);
            if (!contact)
                return;
            if (contact->role == Role::Camera)
            {
                cameraDelta.x += point.x - contact->point.x;
                cameraDelta.y += point.y - contact->point.y;
            }
            contact->point = point;
            UpdateHeldInputs();
        }
        void End(uint64_t id)
        {
            auto* contact = Find(id);
            if (!contact)
                return;
            if (contact->role == Role::Camera)
            {
                cameraDelta = {};
                state.sThumbRX = state.sThumbRY = 0;
            }
            *contact = {};
            UpdateHeldInputs();
        }
        void Tick(double seconds)
        {
            // Convert drag speed to stick deflection, independent of 60/120 Hz.
            const float interval = std::isfinite(seconds) && seconds > 0 ? std::clamp(seconds, 0.001, 0.1) : 1.0 / 60;
            float x = cameraDelta.x * cameraSensitivity / (400 * scale * interval);
            float y = -cameraDelta.y * cameraSensitivity / (400 * scale * interval);
            const float length = std::hypot(x, y);
            if (length > 0)
            {
                // Offset the game's right-stick dead zone so gentle drags still move the camera.
                const float magnitude = 0.27f + 0.73f * std::min(1.0f, length);
                x *= magnitude / length;
                y *= magnitude / length;
            }
            state.sThumbRX = Axis(x);
            state.sThumbRY = Axis(y);
            cameraDelta = {}; // A stationary finger must not keep turning the camera.
        }
    };
}
