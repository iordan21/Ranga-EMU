#ifndef LAUNCHER_LIBRARY_HPP
#define LAUNCHER_LIBRARY_HPP

#include "Common.hpp"

#include <string>
#include <vector>

namespace Ranga {

// One game in the library. Its files stay in folder and are only read; everything made from them, its saves, caches and
// logs live in its own workspace, games\<title id> beside this program. A game of another console has its library key
// in titleId (its own title id for PS4 and PS3, otherwise the console and a hash of its path), and folder can be a single
// file: a disc image or a cartridge dump.
struct Game {
    std::wstring titleId;
    std::wstring title;
    std::wstring version;
    std::wstring folder;
    std::wstring workspace;
    // PS5 games run in Ranga's runtime; any other console (see Systems.hpp) goes to its emulator.
    std::wstring system = L"PS5";
    // The serial read from the disc or its param.sfo, shown beside the title; may be empty.
    std::wstring serial;
};

// Whether the game's files are still where the library points.
bool GameAvailable(const Game& game);

// What the runtime needs to know about a game beyond its files. Only God of War Sons of Sparta has been tested; any other
// game gets the generic profile and is shown as experimental.
struct Profile {
    bool tested = false;
    // Bloodborne (PS4, CUSA03173) does not run in the PS5 runtime: the native port in engines\bloodborne runs it.
    bool bloodborne = false;
    // Sons of Sparta's own options: resolution in its save, skip intro and dialogue, cheats.
    bool sonsOfSparta = false;
    std::vector<std::wstring> excludedModules;
    std::vector<std::wstring> deferredModules;
    // Runtime settings this game needs, set before the game starts; debug_env.txt can still override them.
    std::vector<std::pair<std::wstring, std::wstring>> environment;
};

Profile ProfileFor(const std::wstring& titleId);

// The folder of the Bloodborne port beside this program, with its run.bat, scripts and out\bbport.exe.
std::wstring BloodborneEngine(const std::wstring& root);

// Reads title id, title and version from sce_sys\param.json, or from sce_sys\param.sfo for a PS4 game, and sets system;
// false when the folder has neither.
bool ReadGameInfo(const std::wstring& folder, Game& game);

// The string value of the first "key" at or after from, with the JSON escapes param.json uses; empty when there is none.
std::string JsonString(const std::string& text, const std::string& key, size_t from = 0);

class Library {
public:
    explicit Library(std::wstring root);
    const std::wstring& Root() const { return root; }
    std::vector<Game>& Games() { return games; }
    void Load();
    // Adds the game in folder, or points an existing entry with the same title id at it; returns its index or -1.
    int Add(const std::wstring& folder, std::wstring& error);
    // Adds a game file of another console, once its console is known.
    int AddFile(const std::wstring& path, const std::wstring& system, const std::wstring& serial, const std::wstring& title, std::wstring& error);
    // Adds every game file in folder and the folders right inside it whose console the file itself tells; a .bin or
    // .img with a cue sheet of the same name goes in through the cue sheet. Counts what went in, what fits several
    // consoles (to add one by one) and what no console takes.
    void AddRomFolder(const std::wstring& folder, int& added, int& ambiguous, int& unknown);
    // Points an entry of another console at the place its files moved to, keeping its key and workspace.
    void Relocate(const std::wstring& titleId, const std::wstring& path);
    void Remove(size_t index);
    // Where a console's games can simply be dropped: jogos\<the console's name> beside this program.
    std::wstring GamesFolder(const std::wstring& system) const;
    // Makes the folder of every console (or just of only) and adds the games in them the library does not have yet:
    // files, files one folder down, and game folders where the console has them. The folder says the console, so a
    // file several consoles take needs no asking. A Switch update or add-on is no game: it goes on the list of the
    // game it belongs to (the eden_conteudo core option) when that game is in the library. Returns how many games
    // went in.
    int ScanGamesFolders(const std::wstring& only = L"");
    std::wstring Selected() const;
    void Select(const std::wstring& titleId);

private:
    std::wstring IniPath() const { return root + L"\\library.ini"; }
    int Store(const std::wstring& key, const std::wstring& path, const std::wstring& system, const std::wstring& serial, const std::wstring& title, std::wstring& error);
    std::wstring root;
    std::vector<Game> games;
};

// Per-game options, kept in options.ini in the game's workspace.
struct Options {
    bool borderless = false;
    // resolutionIndex in Sons of Sparta's settings: 0 is 3840 x 2160, 6 is 2560 x 1440, 8 is 1920 x 1080, -1 keeps the game's choice.
    int resolution = -1;
    bool skipIntro = false;
    bool skipDialogue = false;
    bool godMode = false, infiniteSpirit = false, infiniteMagic = false, movementSpeed = false, jumpHeight = false;
    int damageMultiplier = 1;
    // Bloodborne port: BB_FPS (30, 60, 90 or uncap) and BB_RENDER_RES (empty is its native 1920x1080).
    std::wstring fps = L"60";
    std::wstring renderResolution;
    // DLSS 5 through ReShade, with the files the user put in addons\dlss5 (see Dlss5.hpp).
    bool dlss5 = false;
};

Options LoadOptions(const Game& game);
void SaveOptions(const Game& game, const Options& options);

// One value of the game's core-options.ini, the file Ranga Retro hands its core (lines of key = "value", UTF-8);
// empty when the key is not there. Writing keeps every other line as it was.
std::string ReadCoreOption(const Game& game, const std::string& key);
void WriteCoreOption(const Game& game, const std::string& key, const std::string& value);

// An add-on (DLC) dumped from the console. Its files stay in folder; installing it lists its entitlement label in the
// workspace's anyps5-entitlements.ini, which the runtime reads as owned add-ons, and, when it has data beyond sce_sys,
// links addcont\<label> in the workspace to folder, where the runtime mounts it for the game.
struct Addon {
    std::wstring label;
    std::wstring title;
    std::wstring folder;
    bool hasData = false;
};

std::vector<Addon> LoadAddons(const Game& game);
// Empty on success, otherwise why the folder was not installed.
std::wstring InstallAddon(const Game& game, const std::wstring& folder);
void RemoveAddon(const Game& game, const std::wstring& label);

}

#endif
