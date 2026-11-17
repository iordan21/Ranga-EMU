#ifndef LAUNCHER_CONSOLEGRAPHICS_HPP
#define LAUNCHER_CONSOLEGRAPHICS_HPP

// The graphics options on a game's page for the consoles Ranga runs itself, as the Switch's: each is the core's own
// option (its key and the values it lists, RANGA_RETRO_LIST_OPTIONS, 17/11/2026), written to the game's
// core-options.ini, or Ranga Retro's own (ranga_filtro, ranga_escala) for the consoles whose frames Ranga draws.
// "auto" is not written as a value the core sees: it means the core's default or what the PC's Qualidade sets.

#include <string>
#include <utility>
#include <vector>

namespace Ranga {

struct GraphicsOption {
    const char* group;
    const char* key;
    const char* label;
    std::vector<std::pair<const char*, const char*>> values;
    const char* note;
};

// The options for the console's native core, or null when it has none here (the Switch has its own list).
const std::vector<GraphicsOption>* ConsoleGraphics(const std::wstring& system);

}

#endif
