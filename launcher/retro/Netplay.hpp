#ifndef LAUNCHER_RETRO_NETPLAY_HPP
#define LAUNCHER_RETRO_NETPLAY_HPP

// Ranga's online play for cores that have none of their own (NES to PS1 and N64): every player runs the same game, and
// only the buttons travel, through the table server (tools\ranga-sala.exe, launcher/net/RangaNet.hpp).
//
// Lockstep with a fixed delay: the buttons read at frame f are played at frame f + delay, everywhere. Frame f runs
// once every player's buttons for it are here, so a slow link makes the game wait instead of going apart. The player
// in slot 0 runs the game for the table: whoever sits down later gets its state (the core's save state) and starts
// from there. Every 300 frames slot 0 sends a checksum of its state; a player whose own differs asks for the state
// again, so a core that is not perfectly repeatable heals instead of drifting for good.

#include <cstdint>
#include <string>

namespace Netplay {

// One player's controls for one frame: the RetroPad's 16 buttons and the two sticks (left x, y, right x, y).
struct Input {
    uint16_t mask = 0;
    int16_t axis[4] = {};
};

struct Hooks {
    size_t (*stateSize)() = nullptr;
    bool (*saveState)(void*, size_t) = nullptr;
    bool (*loadState)(const void*, size_t) = nullptr;
    void (*log)(const char* format, ...) = nullptr;
    void (*message)(const std::wstring& text) = nullptr;
};

// Connects to the table server at address ("host:port"; no port is the usual 24873) and sits at the table of game.
// False (and the reason in the log and on screen) when the server cannot be reached; the game then runs alone.
bool Start(const std::string& address, const std::string& game, const std::string& name, int delay, const Hooks& hooks);
bool Active();
// Whether this player runs the game for the table (slot 0): its saves are its own; the others' are left alone.
bool Hosting();
// Moves what the network has: sends what is queued and handles what arrived. Called on every turn of the main loop.
void Pump();
// Queues this player's controls, read now, for the frames ahead of the one about to run.
void Send(const Input& local);
// Whether the frame about to run has everyone's controls (and, for a newcomer, the state to start from).
bool Ready();
// The controls of the player in port for the frame running now.
const Input& InputOf(unsigned port);
// After the core ran the frame: the next one, and the state checks.
void FrameDone();
// The host's game changed by itself (reset): everyone gets its state again.
void Resync();
// What the window title shows: who is in, or what it waits for. Empty when nothing is worth saying.
std::wstring Status();
void Stop();

}

#endif
