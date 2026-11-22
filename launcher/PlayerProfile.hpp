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
    // Playing online: the invite to the same game and room ("[[convite|...]]", Client.cpp), for "Entrar no jogo".
    std::string invite;
    // The Ranga version it runs ("1.4.0"); empty: 1.3.0 or older, from before cards said it.
    std::string version;
    bool closed = false; // its Ranga said it was closing
    std::string account;  // Nome#1234, as the central names the account
    std::vector<PlayedGame> recent;
    std::vector<ShowcaseItem> showcase;
    // The games in the player's library (console id, title), so friends see who has what; at most 400.
    std::vector<std::pair<std::string, std::string>> owned;
    // The versions of the photo and cover the central has for this player (Media.hpp "RANI"); empty: none.
    std::string photoVersion, coverVersion;
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
    // The photo and cover ("RANI", Media.hpp) in perfil\\foto.rani and capa.rani, and what the central got last.
    std::string photo, cover, photoSent, coverSent;
    // Their LocalHash, worked out when they change (never on every frame: they can be close to 1 MB).
    std::string photoHash, coverHash;
    std::string raUser, raToken;
    // The Ranga account (the central on Cloudflare): its id, Nome#1234, and this PC's session key. Empty: not signed in.
    std::string accountId, accountName, session;
    void SetAccount(const std::string& id, const std::string& nameAndTag, const std::string& key);
    void ClearAccount();
    // The name shown: the account's (before the #) once signed in, else the one typed in Editar perfil.
    std::string ShownName() const;
    std::map<std::wstring, PlayedGame> games;
    // This player's library (console id, title), kept up to date by the client; goes in the card as owned.
    std::vector<std::pair<std::string, std::string>> owned;
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
    void SetPhoto(const std::string& rani);  // empty removes it
    void SetCover(const std::string& rani);
    // The central has this photo or cover now (its local hash, LocalHash).
    void MarkSent(bool coverToo, const std::string& hash);
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
// A short fingerprint of bytes (FNV-1a 64, hex), to tell whether the photo or cover changed since it was sent.
std::string LocalHash(const std::string& bytes);
std::string Unbase64(const std::string& text);

// Signs in to RetroAchievements with the password (it is not kept): the token comes back, or why not in error.
bool RetroAchievementsLogin(const std::string& user, const std::string& password, std::string& token, std::string& userOut, std::string& error);

// The friends' central (engines.ini [Ranga] central): the Cloudflare one unless another is chosen; "nenhuma" turns it
// off.
std::string CentralServer(const std::wstring& root);
void SetCentralServer(const std::wstring& root, const std::string& address);

// ---- The Ranga account, on the central (launcher/net/cloudflare) ----

struct AccountAnswer {
    bool ok = false;
    int status = 0;
    std::string error, id, name, session, recovery;
    int tag = 0;
};
// This PC, for the central's "3 accounts per PC": SHA-256 of Windows' MachineGuid (it stays until Windows is
// installed again) with Ranga's own prefix, in hex. Never the identifier itself, and the central hashes it again with
// a secret of its own. Empty when Windows does not say.
std::string PcFingerprint();
// The proof of work for creating an account: a number that, after "challenge:", gives a SHA-256 starting with `bits`
// zero bits (a few seconds on a common PC). Empty if it was not found.
std::string SolveChallenge(const std::string& challenge, int bits);
// The owner of the Ranga: the key in salas\\chave-dono.txt (only on the owner's PC), and a call with it.
std::string OwnerKey(const std::wstring& root);
HttpResult OwnerCall(const std::wstring& root, const std::string& method, const std::string& path, const std::string& body);
// POST to /conta/criar, /conta/entrar or /conta/recuperar with "campo<TAB>valor" lines.
AccountAnswer AccountRequest(const std::wstring& root, const std::string& path, const std::string& body);
// Any other call to the central, signed in with session.
HttpResult CentralCall(const std::wstring& root, const std::string& method, const std::string& path, const std::string& body, const std::string& session,
                       int timeoutMs);
// The value of "key<TAB>value" in a reply.
std::string ReplyField(const std::string& text, const std::string& key);
// GET /amigos: friends (with their cards), requests received and sent, as id and Nome#1234.
struct FriendsList {
    std::vector<PlayerCard> friends;
    std::vector<std::pair<std::string, std::string>> incoming, outgoing, blocked;
};
FriendsList ReadFriends(const std::string& text);

// The user agent RetroAchievements is told; it names Ranga and the rcheevos version.
inline constexpr const char* kRaUserAgent = "RangaEmu/2026.11 rcheevos/12.5.0";

}

#endif
