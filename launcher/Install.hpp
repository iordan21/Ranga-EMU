#pragma once

#include <string>

namespace Ranga {

// The RangaEmu.exe that came alone (downloaded from the Releases, no Ranga folder around it): before the window, it
// offers to install itself in C:\Jogos\Ranga (or a folder the person picks), copies itself there, makes the shortcuts
// on the Desktop and in the Start menu and opens that copy. The rest (ranga_retro.exe and the cores) comes from the
// update the first time that copy runs (FirstInstall). True: this process is done and should close.
//
// Only a fresh folder is asked (no engines.ini beside the exe), so a Ranga in use never sees it. RANGA_INSTALAR_PASTA
// (tests) installs there without asking.
bool OfferInstall(const std::wstring& root);

// The folder has no ranga_retro.exe yet: the first update brings everything, on its own.
bool FirstInstall(const std::wstring& root);

}
