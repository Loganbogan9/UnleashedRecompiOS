#pragma once
static constexpr int FPS_MIN = 15;
static constexpr int FPS_MAX = 241;
struct Config
{
    static inline bool AllowBackgroundInput = false;
    static inline int FPS = 120;
};
