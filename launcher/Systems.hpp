#ifndef LAUNCHER_SYSTEMS_HPP
#define LAUNCHER_SYSTEMS_HPP

#include "Common.hpp"

#include <string>
#include <vector>

// The consoles whose games other emulators run for the client. PS5 games go to Ranga's own runtime and Bloodborne, the only
// PS4 game it runs, as an experiment, to its native port; everything else is started through an emulator the user downloaded and pointed at, never one the client
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
    // Ranga Retro does not run its console (or runs it with fewer features), so its game window is shown inside Ranga's
    // own window (Embed.hpp), which also takes over fullscreen.
    bool embed = false;
};

struct System {
    std::wstring id;
    std::string name;
    // Lower-case extensions its games can have, each with its dot.
    std::vector<std::wstring> extensions;
    // Its games can also be a folder: a PS3 dump, an extracted Xbox 360 or Wii U game.
    bool folders = false;
    std::vector<Preset> presets;
    // What the emulator needs besides the game, shown on the Emulators page.
    std::string note;
    // The libretro core Ranga Retro runs its games with, from cores beside this program; empty when Ranga cannot run
    // the console itself yet.
    std::wstring nativeCore = L"";
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
    bool embed = false;
    // Ranga Retro runs the game inside Ranga with the console's core, instead of another emulator.
    bool native = false;
    // The console has a core for Ranga Retro, whether or not it is in use (engines.ini mode=external turns it off).
    bool nativeAvailable = false;
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
// The whole command line, the executable quoted first. {title} and {workspace} are the game's.
std::wstring EngineCommand(const Engine& engine, const std::wstring& rom, bool fullscreen, const std::wstring& title, const std::wstring& workspace);
// What the emulator cannot start rom without that has to come from the user's own console (BIOS, keys, firmware), as
// a sentence naming the file and its folder; empty when nothing is missing or the client cannot tell. rom is empty for
// the console's own system.
std::wstring MissingFiles(const std::wstring& root, const std::wstring& system, const Engine& engine, const std::wstring& rom);
// Runs the console in Ranga Retro (native) or in the emulator chosen for it.
void SetNative(const std::wstring& root, const std::wstring& system, bool native);
// The core file Ranga Retro loads for the console, in cores beside this program.
std::wstring NativeCorePath(const std::wstring& root, const System& system);
// The console's button map for Ranga Retro (controles\<console>.ini beside this program): lines of RetroPad button =
// SDL button name, written by the Controle tab; a missing file or line keeps Ranga Retro's default.
std::wstring ControlsFile(const std::wstring& root, const std::wstring& system);
// The Qualidade chosen for this PC, for every console Ranga runs itself: ruim, fraco, bom (the default) or otimo, kept
// in engines.ini [Ranga] and handed to Ranga Retro (--quality), which sets each core's resolution from it.
// Makes again the folder links the Switch, PS3 and Xbox 360 cores need (bios\retroarch -> emuladores) where a copy, a
// zip or a move of the Ranga folder lost them or left them pointing at the old place.
void RepairLinks(const std::wstring& root);
// The Ranga folder moved (a copy, a zip extracted elsewhere): the paths of the old folder in engines.ini, library.ini,
// the games' options and the emulators' settings become the new folder's, each file kept first in
// backups\mudou-de-pasta-<date>. Remembers the folder in engines.ini [Ranga] raiz. Returns how many files changed.
int RelocateRoot(const std::wstring& root);
std::wstring Quality(const std::wstring& root);
// PS4 and PS5 (God of War Sons of Sparta through AnyPS5, Bloodborne through its port) are experiments kept out of
// sight: off unless engines.ini [Ranga] ps4ps5=1 (RangaEmu.exe --mostrar-ps4-ps5; --esconder-ps4-ps5 hides them).
// Hidden, they have no card on the home screen and no folder in jogos; their games and code stay as they are.
bool ShowPs4Ps5(const std::wstring& root);
void SetShowPs4Ps5(const std::wstring& root, bool on);
void SetQuality(const std::wstring& root, const std::wstring& level);
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
// PS3, Xbox 360 and Wii U game folders; PS5 folders and Bloodborne's are read by ReadGameInfo.
Detected DetectFolder(const std::wstring& folder);
// The file the emulator opens for a game kept at path: the file itself, or the executable inside a game folder. Empty
// when it is not there.
std::wstring GameRom(const std::wstring& system, const std::wstring& path);
// Filters for the file picker: every extension any console takes, then one entry per console.
std::vector<std::pair<std::wstring, std::wstring>> GameFileFilters();
// Takes the icon and background out of a PSP disc image into folder, as ICON0.PNG and PIC1.PNG.
void ExtractArt(const std::wstring& system, const std::wstring& path, const std::wstring& folder);
// What a Switch file holds, told by the title id in its ticket's name (NSP) or between brackets in the file's own name:
// the game itself (an id ending in 000), its update (800) or an add-on (anything else). With no id to read, a name
// saying "update" or "dlc" still tells the kind. baseId is the id of the game it belongs to; ids are 16 upper-case hex
// digits, empty when unknown.
enum class SwitchKind { Unknown, Game, Update, Addon };
struct SwitchContent {
    SwitchKind kind = SwitchKind::Unknown;
    std::wstring titleId, baseId;
};
SwitchContent DetectSwitchContent(const std::wstring& path);
// The library key of a game file: console and a hash of its path, so the same file is added once.
std::wstring FileKey(const std::wstring& system, const std::wstring& path);
// An Xbox 360 game's title id ("4D5307E6"), read from its default.xex (folder, loose .xex or disc image); empty if none.
std::wstring X360TitleId(const std::wstring& path);

}

#endif
