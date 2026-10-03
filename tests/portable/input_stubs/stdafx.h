#pragma once
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstring>
#include <thread>
#include <xbox.h>
#include <SDL.h>
#include <sdl_events.h>

template<typename T>
constexpr size_t FirstBitLow(T value) { return std::countr_zero(static_cast<std::make_unsigned_t<T>>(value)); }
