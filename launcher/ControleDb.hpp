#pragma once

namespace Ranga {

// Teaches SDL2 the community's list of gamepads (SDL_GameControllerDB, the Windows lines, built into the exe by
// tools/gerar_controles.py), so cheap generic pads work as a gamepad too. Call after SDL_INIT_GAMECONTROLLER, before
// opening the pads. Returns how many mappings went in, or -1.
int AddKnownPads();

}
