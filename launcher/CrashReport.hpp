#ifndef LAUNCHER_CRASHREPORT_HPP
#define LAUNCHER_CRASHREPORT_HPP

// "Enviar ao Ranga": a game that closed with an error, sent to the Ranga's project in Sentry (sentry.io) so the owner
// sees which emulator, console and version are breaking. Only when the player presses the button: nothing goes by
// itself, and Ranga Alpha sends nothing. What goes: Ranga's version, the console, the emulator, the game's name and
// serial, the exit code, the seconds it ran and the text logs of that run, with the Windows user name, the PC's name,
// e-mails, IP addresses and secrets taken out. No saves, keys, BIOS, games or account.
//
// No Sentry library: one HTTPS request with an "envelope" (Sentry's format), through Http.cpp.

#include <string>
#include <utility>
#include <vector>

namespace Ranga::CrashReport {

struct Report {
    std::string version;  // "1.8.2"
    std::string update;   // the update's stamp
    std::string console;  // "Nintendo Switch"
    std::string emulator; // "Eden"
    std::string game;
    std::string serial;
    unsigned long code = 0; // NTSTATUS, 0xC0000005 and the like
    unsigned long seconds = 0;
    std::string where;    // "pcsx2_libretro.dll+0x1A2B3C": where Ranga Retro saw it crash (retro/Queda.hpp), if it did
    std::vector<std::pair<std::string, std::string>> logs; // file name, text (cleaned before it goes)
};

// The project's DSN; RANGA_SENTRY_DSN overrides it (tests). Empty: sending is off and the button does not show.
std::string Dsn();
bool Enabled();
// The text with the user's name, the PC's name, the user folder, e-mails, IPv4 addresses and "token=..."-like secrets
// replaced by <usuario>, <pc>, <email>, <ip> and <segredo>.
std::string Clean(const std::string& text);
// From a game's log, the place of the last "[queda] codigo=0x... em <place>" line Ranga Retro wrote; empty if none.
std::string CrashPlace(const std::string& log);
// The whole envelope, as it goes (public for the tests).
std::string Envelope(const Report& report, const std::string& dsn, const std::string& eventId);
// Sends it; blocks (run it off the drawing thread). Empty: sent; otherwise what went wrong, in Portuguese.
std::wstring Send(const Report& report);

}

#endif
