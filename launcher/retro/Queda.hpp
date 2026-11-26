#ifndef LAUNCHER_RETRO_QUEDA_HPP
#define LAUNCHER_RETRO_QUEDA_HPP

// When Ranga Retro crashes for real (not the faults a core makes on purpose and handles itself, like PCSX2's fast
// memory), this writes where it happened to the game's log, so the launcher's "Enviar ao Ranga" carries it:
//
//   [queda] codigo=0xC0000005 em pcsx2_libretro.dll+0x1A2B3C (leitura em 0x0000000000000000)
//   [queda] pilha: pcsx2_libretro.dll+0x1A2B3C < pcsx2_libretro.dll+0x99887 < ranga_retro.exe+0x4455 ...
//
// Only module names and offsets: no paths, so nothing of the user's goes. A minidump (queda.dmp) is also written to
// the game's logs folder; it stays on the PC (it can hold anything that was in memory) and is never sent.

#include <string>

namespace Ranga::Retro {

// logsFolder: where queda.dmp goes. Call once at start, and again after the core loads its game (a core may put its
// own last-chance handler in place of Ranga's).
void InstallCrashCapture(const std::wstring& logsFolder);
void ReinstallCrashCapture();

}

#endif
