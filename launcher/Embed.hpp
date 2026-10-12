#ifndef LAUNCHER_EMBED_HPP
#define LAUNCHER_EMBED_HPP

#include "Common.hpp"

#include <string>

// Shows another emulator's game inside a window of Ranga's own, for the consoles no libretro core covers (PS3, Xbox 360,
// and Switch, Wii U, Xbox with mode=external). The emulator's largest window is made a borderless, menuless child of the host window,
// which takes the game's title, its place on the taskbar and its close button.
namespace Ranga {

// Runs on the calling thread until the process ends, pumping the host window's messages. fullscreen fills the monitor
// without borders. Closing the host asks the emulator to close; if it is still running ten seconds later, closing again
// offers to end it.
void EmbedUntilExit(const PROCESS_INFORMATION& process, const std::wstring& title, bool fullscreen);

}

#endif
