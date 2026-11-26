#ifndef LAUNCHER_DIAGNOSIS_HPP
#define LAUNCHER_DIAGNOSIS_HPP

#include "Library.hpp"
#include "Systems.hpp"

#include <string>

namespace Ranga {

// "Por que não abriu?": why a game did not open or closed by itself, in plain words, with what can be done at once.
struct Diagnosis {
    enum class Fix { None, OpenFolder, Repair, VcRuntime, Dlss5Off };
    std::string reason;  // Portuguese, a whole text of idiomas.txt; empty: nothing to say
    std::string todo;    // what to do about it (the same)
    std::wstring detail; // file or folder it is about, shown as it is
    bool sure = false;   // read from the file or said by the emulator; otherwise it is the usual cause
    Fix fix = Fix::None;
    std::wstring folder; // Fix::OpenFolder
};

// What is wrong with the game's own file, read from it: gone, empty, kept only in the cloud, a web page saved in its
// place, its disc tracks missing, not the kind its extension says. starting: before it opens, where a file in the cloud
// is left alone (opening it makes Windows fetch it).
Diagnosis CheckGameFile(const std::wstring& rom, bool starting = false);
// After it ran: exit code, seconds open and the logs of that run. Empty reason: it closed as it should.
Diagnosis Diagnose(const std::wstring& root, const Game& game, const Engine& engine, const std::wstring& rom, const Options& options, unsigned long code,
                   unsigned long seconds);

}

#endif
