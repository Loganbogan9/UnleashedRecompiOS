#pragma once
#include <SDL.h>

// The host SDL headers omit iOS API declarations. The test supplies the
// animation scheduler while the production callback uses real SDL input.
extern "C" int SDL_iPhoneSetAnimationCallback(SDL_Window*, int, void (*)(void*), void*);
