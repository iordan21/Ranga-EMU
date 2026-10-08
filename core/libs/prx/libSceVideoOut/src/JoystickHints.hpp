#ifndef CORE_LIBS_PRX_LIBSCEVIDEOOUT_SRC_JOYSTICKHINTS_HPP
#define CORE_LIBS_PRX_LIBSCEVIDEOOUT_SRC_JOYSTICKHINTS_HPP

#include "SDL.h"

// Must run before the first SDL_InitSubSystem with SDL_INIT_GAMECONTROLLER or SDL_INIT_JOYSTICK.
// SDL's DirectInput and Windows.Gaming.Input backends start COM on the calling thread while XInput1_4 may be starting
// InputHost on a thread pool thread, and InputHost needs COM too: the two can wait on each other forever, and the game
// then hangs before its window opens. DirectInput enumeration can also block forever on some HID devices (AnyPS5 issue
// #1042). DualSense and DualShock pads use HIDAPI and Xbox pads use XInput and RawInput, so neither backend is needed;
// SDL_DIRECTINPUT_ENABLED=1 or SDL_JOYSTICK_WGI=1 in the environment (debug_env.txt) turns one back on.
inline void SetJoystickBackendHints() {
    SDL_SetHint(SDL_HINT_DIRECTINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
}

#endif
