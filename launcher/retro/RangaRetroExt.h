#pragma once

// Ranga Retro's own environment commands, in libretro's private range. A core that presents through its own GPU device
// (the Switch core, whose renderer makes its device and swapchain on a window) asks for the game window and draws in it;
// Ranga then keeps the window, the gamepads, the menu and the sound, and draws nothing itself.
#define RANGA_ENVIRONMENT_GET_WINDOW (RETRO_ENVIRONMENT_PRIVATE | 0x5241) // HWND*: the game window
#define RANGA_ENVIRONMENT_SET_FPS (RETRO_ENVIRONMENT_PRIVATE | 0x5242)    // const float*: frames shown per second
