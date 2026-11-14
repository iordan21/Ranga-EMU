#pragma once

// Ranga Retro's own environment commands, in libretro's private range. A core that presents through its own GPU device
// (the Switch core, whose renderer makes its device and swapchain on a window) asks for the game window and draws in it;
// Ranga then keeps the window, the gamepads, the menu and the sound, and draws nothing itself.
#define RANGA_ENVIRONMENT_GET_WINDOW (RETRO_ENVIRONMENT_PRIVATE | 0x5241) // HWND*: the game window
#define RANGA_ENVIRONMENT_SET_FPS (RETRO_ENVIRONMENT_PRIVATE | 0x5242)    // const float*: frames shown per second
// ranga_text_request*: asks the player for a line of text (a game's keyboard), on a box Ranga draws over the game with
// its own on-screen keyboard (gamepad, mouse or the PC's keyboard). It returns once the player is done; accepted says
// whether they confirmed. Strings are UTF-8.
#define RANGA_ENVIRONMENT_GET_TEXT (RETRO_ENVIRONMENT_PRIVATE | 0x5243)

struct ranga_text_request {
    const char* title;   // what the game asks for; may be empty
    const char* guide;   // a line under the box; may be empty
    const char* initial; // the text the box starts with
    unsigned max_length; // in UTF-16 units; 0 is no limit
    int password;        // the text is shown as dots
    char* result;        // filled with the text, zero-ended
    unsigned result_size;
    int accepted;        // 1 confirmed, 0 cancelled
};
