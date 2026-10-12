#ifndef LAUNCHER_SYSTEMS_HPP
#define LAUNCHER_SYSTEMS_HPP

#include "Common.hpp"

#include <string>
#include <vector>

// The consoles whose games other emulators run for the client. PS5 games go to Ranga's own runtime and Bloodborne to its
// native port; everything else is started through an emulator the user downloaded and pointed at, never one the client
// fetches itself.
namespace Ranga {

// One setting written into an emulator's own configuration file when the user asks the client to fit it to this PC.
struct Tweak {
    // Relative to the emulator's folder.
    std::wstring file;
    // The [section] it belongs to; empty for a file without sections, like retroarch.cfg.
    std::string section;
    std::string key;
    // Written as it is, quotes included; {root} becomes the client's folder.
    std::string value;
};

// An emulator the client knows the command line of.
struct Preset {
    std::wstring name;
    // Lower-case beginnings of its executable's file name, separated by ';'.
    std::wstring exePrefixes;
    // {rom} is the game's file, {romdir} its folder, {romname} its name without extension, {exedir} the emulator's folder
    // and {fullscreen} the fullscreen argument when the game's screen option asks for it.
    std::wstring args;
    std::wstring fullscreen;
    // Settings that fit it to this PC: Vulkan where it has it, a higher internal resolution, no first-run wizard, the
    // first SDL gamepad (DualSense, Xbox or generic) on player 1 where its bindings do not name the device.
    std::vector<Tweak> tweaks = {};
    // Its BIOS folder under bios beside this program, which the tweaks point it at; empty when it needs none.
    std::wstring bios = L"";
};

struct System {
    std::wstring id;
    std::string name;
    // Lower-case extensions its games can have, each with its dot.
    std::vector<std::wstring> extensions;
    // Its games can also be a folder: a PS4 or PS3 dump, an extracted Xbox 360 or Wii U game.
    bool folders = false;
    std::vector<Preset> presets;
    // What the emulator needs besides the game, shown on the Emulators page.
    std::string note;
};

// Every console handed to another emulator, in the order the Emulators page lists them.
const std::vector<System>& Systems();
const System* FindSystem(const std::wstring& id);
// The console's name for the screen, PS5 included.
std::string SystemName(const std::wstring& id);

// The emulator chosen for a console, kept in engines.ini beside this program.
struct Engine {
    std::wstring exe;
    std::wstring args;
    std::wstring fullscreen;
    // The preset the executable's name matched; empty when the client does not know it and passes just the file.
    std::wstring presetName;
    // The matched preset has settings to fit this PC, and the folder where its BIOS goes (empty when none).
    bool tunable = false;
    std::wstring bios;
    bool ready = false;
    // Why the game cannot start yet; empty when ready.
    std::wstring problem;
};

Engine LoadEngine(const std::wstring& root, const std::wstring& system);
// Remembers exe for the console with the arguments of the preset its name matches.
void ChooseEngine(const std::wstring& root, const std::wstring& system, const std::wstring& exe);
void SaveEngineArgs(const std::wstring& root, const std::wstring& system, const std::wstring& args, const std::wstring& fullscreen);
void ResetEngineArgs(const std::wstring& root, const std::wstring& system);
void ClearEngine(const std::wstring& root, const std::wstring& system);
// The whole command line, the executable quoted first.
std::wstring EngineCommand(const Engine& engine, const std::wstring& rom, bool fullscreen);
// Writes the preset's tweaks into the emulator's configuration, which has to be closed, and makes its BIOS folder.
// Returns what was changed, one line each, or why nothing could be.
std::wstring TuneEngine(const std::wstring& root, const std::wstring& system);

// What a game file or folder turned out to be.
struct Detected {
    // One console when it is known, several when the user has to choose, none when nothing takes it.
    std::vector<std::wstring> systems;
    std::wstring serial;
    std::wstring title;
};

Detected DetectFile(const std::wstring& path);
// PS3, Xbox 360 and Wii U game folders; PS5 and PS4 folders are read by ReadGameInfo.
Detected DetectFolder(const std::wstring& folder);
// The file the emulator opens for a game kept at path: the file itself, or the executable inside a game folder. Empty
// when it is not there.
std::wstring GameRom(const std::wstring& system, const std::wstring& path);
// Filters for the file picker: every extension any console takes, then one entry per console.
std::vector<std::pair<std::wstring, std::wstring>> GameFileFilters();
// Takes the icon and background out of a PSP disc image into folder, as ICON0.PNG and PIC1.PNG.
void ExtractArt(const std::wstring& system, const std::wstring& path, const std::wstring& folder);
// The library key of a game file: console and a hash of its path, so the same file is added once.
std::wstring FileKey(const std::wstring& system, const std::wstring& path);

}

#endif
