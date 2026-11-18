#ifndef RANGA_RETRO_ACHIEVEMENTS_HPP
#define RANGA_RETRO_ACHIEVEMENTS_HPP

// RetroAchievements in Ranga Retro, through rcheevos (3rdparty/rcheevos, MIT). The player signs in from Ranga's
// profile (the token is in perfil\perfil.txt beside the program); here the game is identified by its hash, the
// achievements are checked against the core's memory every frame, unlocks go to the server, show over the game, and
// the game's list is written to retroachievements.txt in its workspace, where the profile reads it.
// Always softcore: Ranga is not one of the emulators RetroAchievements approved for hardcore.

#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

struct retro_memory_map;

namespace Achievements {

struct Setup {
    std::wstring programFolder; // where perfil\ is
    std::wstring system;        // Ranga's console id (RANGA_SISTEMA)
    std::wstring rom;
    const unsigned char* data = nullptr; // the game in memory, when the core takes it that way
    size_t size = 0;
    std::wstring workspace;
    HWND window = nullptr;
    std::function<void*(unsigned)> memoryData;
    std::function<size_t(unsigned)> memorySize;
    std::function<void(const char*)> log;
};

// Starts when the player is signed in and the console has achievements; otherwise nothing happens at all.
void Start(const Setup& setup);
bool Active();
// The core's memory map (RETRO_ENVIRONMENT_SET_MEMORY_MAPS), kept for reading the game's memory.
void SetMemoryMap(const retro_memory_map* map);
// After every frame the core ran.
void Frame();
// Every turn of the loop, paused or not: the server's answers and the notices over the game.
void Idle();
// The game was reset or a state was loaded: the achievements start watching from scratch.
void Reset();
void Stop();

}

#endif
