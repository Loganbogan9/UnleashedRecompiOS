#pragma once
#include <atomic>
#include <SDL.h>
struct GameWindow
{
    static inline SDL_Window* s_pWindow = nullptr;
    static inline std::atomic<bool> s_isFocused = true;
    static inline bool s_isFullscreenCursorVisible = false;
    static inline unsigned updates = 0;
    static bool IsFullscreen() { return false; }
    static void Update() { ++updates; }
};
