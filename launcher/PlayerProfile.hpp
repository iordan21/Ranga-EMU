#ifndef LAUNCHER_PLAYERPROFILE_HPP
#define LAUNCHER_PLAYERPROFILE_HPP

// The player's profile, Steam-style: name, phrase and photo, the time played in each game, Ranga's own achievements
// (worked out from that time, on every console), the RetroAchievements Ranga Retro unlocked in each game, and a level
// from all of it. Everything lives in perfil\ beside the program; only the card (PlayerCard) leaves the PC, to the
// hosting PC's room, where friends see it. The RetroAchievements token never leaves.

#include "Http.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Ranga {

struct PlayedGame {
    std::wstring key, system, title;
    int64_t seconds = 0, sessions = 0, first = 0, last = 0;
};

// One achievement of a game on RetroAchievements, as Ranga Retro wrote it (workspace\retroachievements.txt).
struct RaAchievement {
    uint32_t id = 0;
    int points = 0;
    int64_t unlocked = 0; // 0: still locked
    std::string badge, title, description;
};

struct RaGame {
    std::wstring key, system;
    uint32_t id = 0;
    std::string title, badge;
    std::vector<RaAchievement> achievements;
    int Unlocked() const;
};

// One of Ranga's own achievements and how far the player is.
struct RangaAchievement {
    const char* id;
    const char* title;
    const char* description;
    int goal = 1, progress = 0;
    int64_t unlocked = 0;
};

// What shows in the showcase: Ranga's own (badge is its id) or RetroAchievements' (badge is the image's name).
struct ShowcaseItem {
    bool ra = false;
    int64_t when = 0;
    int points = 0;
    std::string badge, title, description;
};

// The profile as it is shown: this player's, or a friend's as the room handed it over.
struct PlayerCard {
    std::string id, name, phrase;
    int64_t seconds = 0, seen = 0;
    int xp = 0, games = 0, consoles = 0, achievements = 0, raPoints = 0;
    std::string playing, playingSystem;
    bool closed = false; // its Ranga said it was closing
    std::vector<PlayedGame> recent;
    std::vector<ShowcaseItem> showcase;
    std::string avatar; // kAvatarSide x kAvatarSide RGBA, or empty
};

inline constexpr int kAvatarSide = 128;

struct Level {
    int level = 0, into = 0, size = 100;
};
Level LevelFor(int xp);

class PlayerProfile {
public:
    void Load(const std::wstring& root);
    const std::wstring& Root() const { return root; }

    std::string id, secret, name, phrase;
    std::string avatar;
    std::string raUser, raToken;
    std::map<std::wstring, PlayedGame> games;
    std::vector<RaGame> raGames;
    std::vector<RangaAchievement> achievements;

    // A game closed after seconds; online when it ran with online play on. Saves at once.
    void RecordSession(const std::wstring& key, const std::wstring& system, const std::wstring& title, int64_t started, int64_t seconds, bool online);
    // Reads every game's retroachievements.txt (workspaces as the library has them).
    void ScanRetroAchievements(const std::vector<std::pair<std::wstring, std::wstring>>& keyAndWorkspace,
                               const std::map<std::wstring, std::pair<std::wstring, std::wstring>>& keySystemTitle);
    // Works every achievement's progress out again; those reached now are unlocked, saved and returned (titles).
    std::vector<std::string> Evaluate(int libraryCount);

    void SetName(const std::string& text);
    void SetPhrase(const std::string& text);
    void SetAvatar(const std::string& rgba); // kAvatarSide square, or empty to remove
    void SetRetroAchievements(const std::string& user, const std::string& token);

    int64_t TotalSeconds() const;
    int Consoles() const;
    int RangaUnlocked() const;
    int RaUnlocked() const;
    int RaPoints() const;
    int Xp() const;
    // This player's card; playing is the game open now (empty when none).
    PlayerCard Card(const std::string& playing, const std::string& playingSystem) const;
    // When the room should be told again (the card changed since it was last sent).
    uint64_t Revision() const { return revision; }

private:
    void SaveMain() const;
    void SaveGames() const;
    void SaveUnlocked() const;
    std::wstring root, folder;
    std::set<int> days;            // yyyymmdd played
    int64_t longest = 0, online = 0;
    bool nightOwl = false;
    std::map<std::string, int64_t> unlocked;
    uint64_t revision = 1;
};

// The card as text for the room, and the room's list of cards back (each starts at "ranga-perfil 1").
std::string WriteCard(const PlayerCard& card);
std::vector<PlayerCard> ReadCards(const std::string& text);

std::string Base64(const std::string& bytes);
std::string Unbase64(const std::string& text);

// Signs in to RetroAchievements with the password (it is not kept): the token comes back, or why not in error.
bool RetroAchievementsLogin(const std::string& user, const std::string& password, std::string& token, std::string& userOut, std::string& error);

// Where the profiles are kept: engines.ini [Ranga] perfis, else this PC's room when it hosts, else the update's source.
std::string ProfilesServer(const std::wstring& root);

// The friends' central server (engines.ini [Ranga] central): one always on, such as Oracle Cloud's, tried first; when it
// does not answer, ProfilesServer takes over for a few minutes. Empty: only ProfilesServer.
std::string CentralServer(const std::wstring& root);
void SetCentralServer(const std::wstring& root, const std::string& address);
// The group's code (engines.ini [Ranga] grupo): letters and digits; a server started with --grupo answers only with it.
std::string GroupCode(const std::wstring& root);
void SetGroupCode(const std::wstring& root, const std::string& code);

struct SocialAnswer {
    HttpResult result;
    std::string server; // the one that answered (or the last one tried)
};
// A request to the friends' server (profiles, chat): the central one first, then the hosting PC's room.
SocialAnswer SocialRequest(const std::wstring& root, const std::string& method, const std::string& path, const std::string& body, int timeoutMs);

// The user agent RetroAchievements is told; it names Ranga and the rcheevos version.
inline constexpr const char* kRaUserAgent = "RangaEmu/2026.11 rcheevos/12.5.0";

}

#endif
