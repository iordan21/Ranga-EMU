#ifndef LAUNCHER_LIBRARY_HPP
#define LAUNCHER_LIBRARY_HPP

#include "Common.hpp"

#include <string>
#include <vector>

namespace Ranga {

// One game in the library. Its files stay in folder and are only read; everything made from them, its saves, caches and
// logs live in its own workspace, games\<title id> beside this program.
struct Game {
    std::wstring titleId;
    std::wstring title;
    std::wstring version;
    std::wstring folder;
    std::wstring workspace;
};

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

// Reads title id, title and version from sce_sys\param.json, or from sce_sys\param.sfo for a PS4 game; false when the
// folder has neither.
bool ReadGameInfo(const std::wstring& folder, Game& game);

class Library {
public:
    explicit Library(std::wstring root);
    const std::wstring& Root() const { return root; }
    std::vector<Game>& Games() { return games; }
    void Load();
    // Adds the game in folder, or points an existing entry with the same title id at it; returns its index or -1.
    int Add(const std::wstring& folder, std::wstring& error);
    void Remove(size_t index);
    std::wstring Selected() const;
    void Select(const std::wstring& titleId);

private:
    std::wstring IniPath() const { return root + L"\\library.ini"; }
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
};

Options LoadOptions(const Game& game);
void SaveOptions(const Game& game, const Options& options);

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
