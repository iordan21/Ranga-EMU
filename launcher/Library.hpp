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
    // Sons of Sparta's own options: resolution in its save, skip intro and dialogue, cheats.
    bool sonsOfSparta = false;
    std::vector<std::wstring> excludedModules;
    std::vector<std::wstring> deferredModules;
};

Profile ProfileFor(const std::wstring& titleId);

// Reads title id, title and version from sce_sys\param.json; false when the folder has no readable param.json.
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
};

Options LoadOptions(const Game& game);
void SaveOptions(const Game& game, const Options& options);

}

#endif
