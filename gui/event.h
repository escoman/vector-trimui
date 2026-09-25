#pragma once

/*
 * SDL event / scancode provider for the TrimUI (SDL2) build.
 *
 * The PSP port shipped a hand-written event.h that faked a minimal
 * subset of the SDL types (typedef int SDL_Scancode plus a private
 * SDL_SCANCODE_* enum) so the GUI code could stay source-compatible
 * without linking SDL. On TrimUI the real SDL2 headers are available,
 * so this shim simply pulls them in: SDL_Scancode and every
 * SDL_SCANCODE_* constant the virtual keyboard emits (including HOME
 * and END for the numpad cluster) come straight from SDL.h and match
 * the values the core keyboard.h maps onto the Vector matrix.
 */

#include "SDL.h"
