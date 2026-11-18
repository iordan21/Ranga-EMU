#include "PlayerProfile.hpp"
#include "Common.hpp"
#include "Http.hpp"
#include "Library.hpp"
#include "Rooms.hpp"
#include "Update.hpp"

#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>

namespace Ranga {

namespace {

enum class Kind { FirstGame, TotalHours, SessionHours, Night, GameHours, Consoles, Library, GamesPlayed, Streak, Online, RaCount, RaMastery, Photo };

struct Definition {
    const char* id;
    const char* title;
    const char* description;
    Kind kind;
    int goal;
};

// Ranga's own achievements: they come from what is played, so every console counts, the Switch and the 3DS too.
const Definition kDefinitions[] = {
    {"primeiro_jogo", "Aperta o start", "Jogou um jogo pela primeira vez no Ranga.", Kind::FirstGame, 1},
    {"foto", "Cara nova", "Colocou uma foto no perfil.", Kind::Photo, 1},
    {"uma_hora", "Esquentando", "Uma hora de jogo, somando tudo.", Kind::TotalHours, 1},
    {"dez_horas", "Jogador de verdade", "Dez horas de jogo, somando tudo.", Kind::TotalHours, 10},
    {"cinquenta_horas", "Sem vida social", "Cinquenta horas de jogo, somando tudo.", Kind::TotalHours, 50},
    {"cem_horas", "Lenda do Ranga", "Cem horas de jogo, somando tudo.", Kind::TotalHours, 100},
    {"maratona", "Maratona", "Três horas seguidas no mesmo jogo.", Kind::SessionHours, 3},
    {"coruja", "Coruja", "Jogou entre meia-noite e cinco da manhã.", Kind::Night, 1},
    {"meu_jogo", "Esse é o meu jogo", "Dez horas num jogo só.", Kind::GameHours, 10},
    {"turista", "Turista", "Jogou em três consoles diferentes.", Kind::Consoles, 3},
    {"viajante", "Viajante do tempo", "Jogou em seis consoles diferentes.", Kind::Consoles, 6},
    {"museu", "Museu do videogame", "Jogou em dez consoles diferentes.", Kind::Consoles, 10},
    {"provador", "Provador", "Jogou dez jogos diferentes.", Kind::GamesPlayed, 10},
    {"estante", "Estante cheia", "Dez jogos na biblioteca.", Kind::Library, 10},
    {"colecionador", "Colecionador", "Cinquenta jogos na biblioteca.", Kind::Library, 50},
    {"semana", "Uma semana sem largar", "Jogou sete dias seguidos.", Kind::Streak, 7},
    {"junto", "Junto é melhor", "Primeira partida online.", Kind::Online, 1},
    {"parceiro", "Parceiro de jogatina", "Dez partidas online.", Kind::Online, 10},
    {"retro_primeira", "Conquista de verdade", "Primeira conquista do RetroAchievements.", Kind::RaCount, 1},
    {"retro_cacador", "Caçador de troféus", "Cinquenta conquistas do RetroAchievements.", Kind::RaCount, 50},
    {"platina", "Platina", "Todas as conquistas do RetroAchievements de um jogo.", Kind::RaMastery, 1},
};

std::vector<std::string> Split(const std::string& line, char separator) {
    std::vector<std::string> parts;
    size_t at = 0;
    for (;;) {
        const size_t end = line.find(separator, at);
        parts.push_back(line.substr(at, end == std::string::npos ? std::string::npos : end - at));
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return parts;
}

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> lines;
    for (std::string& line : Split(text, '\n')) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

// Text in a field of a tab-separated line: no tabs or line breaks of its own.
std::string Clean(std::string text, size_t limit = 200) {
    for (char& c : text) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    if (text.size() > limit) {
        text.resize(limit);
        // Not in the middle of a UTF-8 character.
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xc0) == 0x80) text.pop_back();
        if (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xc0) == 0xc0) text.pop_back();
    }
    return text;
}

int64_t Number(const std::string& text) {
    try {
        return text.empty() ? 0 : std::stoll(text);
    } catch (...) {
        return 0;
    }
}

void WriteWhole(const std::wstring& path, const std::string& bytes) {
    // Written beside and moved over, so a crash halfway never leaves half a file.
    const std::wstring temporary = path + L".novo";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &put, nullptr) && put == bytes.size();
    CloseHandle(file);
    if (ok) MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    else DeleteFileW(temporary.c_str());
}

std::string RandomHex(size_t bytes) {
    std::string raw(bytes, '\0');
    BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(raw.data()), static_cast<ULONG>(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (const unsigned char c : raw) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

int DayOf(int64_t when) {
    const time_t at = static_cast<time_t>(when);
    tm local{};
    localtime_s(&local, &at);
    return (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
}

int HourOf(int64_t when) {
    const time_t at = static_cast<time_t>(when);
    tm local{};
    localtime_s(&local, &at);
    return local.tm_hour;
}

// The day after yyyymmdd.
int NextDay(int day) {
    tm date{};
    date.tm_year = day / 10000 - 1900;
    date.tm_mon = day / 100 % 100 - 1;
    date.tm_mday = day % 100 + 1;
    date.tm_hour = 12;
    mktime(&date);
    return (date.tm_year + 1900) * 10000 + (date.tm_mon + 1) * 100 + date.tm_mday;
}

}

int RaGame::Unlocked() const {
    int count = 0;
    for (const auto& achievement : achievements) count += achievement.unlocked != 0;
    return count;
}

Level LevelFor(int xp) {
    // As on Steam: 100 XP a level up to 10, 200 up to 20, and so on.
    Level level;
    int left = std::max(0, xp);
    for (;;) {
        level.size = 100 * (level.level / 10 + 1);
        if (left < level.size) break;
        left -= level.size;
        ++level.level;
    }
    level.into = left;
    return level;
}

void PlayerProfile::Load(const std::wstring& rootFolder) {
    root = rootFolder;
    folder = root + L"\\perfil";
    MakeFolders(folder);
    *this = PlayerProfile{};
    root = rootFolder;
    folder = root + L"\\perfil";
    for (const std::string& line : Lines(ReadBytes(folder + L"\\perfil.txt"))) {
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string key = line.substr(0, tab), value = line.substr(tab + 1);
        if (key == "id") id = value;
        else if (key == "segredo") secret = value;
        else if (key == "nome") name = value;
        else if (key == "frase") phrase = value;
        else if (key == "ra_usuario") raUser = value;
        else if (key == "ra_token") raToken = value;
        else if (key == "maior_sessao") longest = Number(value);
        else if (key == "online") online = Number(value);
        else if (key == "madrugada") nightOwl = value == "1";
        else if (key == "dias") for (const std::string& day : Split(value, ',')) if (!day.empty()) days.insert(static_cast<int>(Number(day)));
    }
    bool fresh = false;
    if (id.size() != 32) id = RandomHex(16), fresh = true;
    if (secret.size() != 32) secret = RandomHex(16), fresh = true;
    if (name.empty()) {
        // The Windows user's name to start with; Editar perfil changes it.
        wchar_t user[128] = {};
        DWORD size = 128;
        name = GetUserNameW(user, &size) ? Utf8(user) : "Jogador";
        fresh = true;
    }
    if (fresh) SaveMain();
    for (const std::string& line : Lines(ReadBytes(folder + L"\\jogos.txt"))) {
        const auto parts = Split(line, '\t');
        if (parts.size() < 7) continue;
        PlayedGame game;
        game.key = Wide(parts[0]);
        game.system = Wide(parts[1]);
        game.title = Wide(parts[2]);
        game.seconds = Number(parts[3]);
        game.sessions = Number(parts[4]);
        game.first = Number(parts[5]);
        game.last = Number(parts[6]);
        games[game.key] = game;
    }
    for (const std::string& line : Lines(ReadBytes(folder + L"\\conquistas.txt"))) {
        const auto parts = Split(line, '\t');
        if (parts.size() >= 2 && !parts[0].empty()) unlocked[parts[0]] = Number(parts[1]);
    }
    const std::string pixels = ReadBytes(folder + L"\\avatar.rgba");
    if (pixels.size() == static_cast<size_t>(kAvatarSide) * kAvatarSide * 4) avatar = pixels;
    for (const auto& definition : kDefinitions) {
        RangaAchievement achievement{definition.id, definition.title, definition.description, definition.goal};
        if (const auto found = unlocked.find(definition.id); found != unlocked.end()) achievement.unlocked = found->second;
        achievements.push_back(achievement);
    }
}

void PlayerProfile::SaveMain() const {
    std::string text;
    const auto put = [&](const char* key, const std::string& value) { text += std::string(key) + "\t" + Clean(value, 4096) + "\n"; };
    put("id", id);
    put("segredo", secret);
    put("nome", name);
    put("frase", phrase);
    put("ra_usuario", raUser);
    put("ra_token", raToken);
    put("maior_sessao", std::to_string(longest));
    put("online", std::to_string(online));
    put("madrugada", nightOwl ? "1" : "0");
    std::string list;
    for (const int day : days) list += (list.empty() ? "" : ",") + std::to_string(day);
    put("dias", list);
    WriteWhole(folder + L"\\perfil.txt", text);
}

void PlayerProfile::SaveGames() const {
    std::string text;
    for (const auto& [key, game] : games) {
        text += Clean(Utf8(game.key)) + "\t" + Clean(Utf8(game.system)) + "\t" + Clean(Utf8(game.title)) + "\t" + std::to_string(game.seconds) + "\t" +
                std::to_string(game.sessions) + "\t" + std::to_string(game.first) + "\t" + std::to_string(game.last) + "\n";
    }
    WriteWhole(folder + L"\\jogos.txt", text);
}

void PlayerProfile::SaveUnlocked() const {
    std::string text;
    for (const auto& [key, when] : unlocked) text += key + "\t" + std::to_string(when) + "\n";
    WriteWhole(folder + L"\\conquistas.txt", text);
}

void PlayerProfile::RecordSession(const std::wstring& key, const std::wstring& system, const std::wstring& title, int64_t started, int64_t seconds, bool onlineSession) {
    // A game that did not really start (closed at once, failed to load) is no session.
    if (seconds < 20) return;
    PlayedGame& game = games[key];
    if (game.key.empty()) {
        game.key = key;
        game.first = started;
    }
    game.system = system;
    game.title = title;
    game.seconds += seconds;
    game.sessions += 1;
    game.last = started + seconds;
    longest = std::max(longest, seconds);
    if (onlineSession) ++online;
    for (int64_t at = started; at <= started + seconds; at += 1800) {
        days.insert(DayOf(at));
        if (HourOf(at) < 5) nightOwl = true;
    }
    days.insert(DayOf(started + seconds));
    if (HourOf(started + seconds) < 5) nightOwl = true;
    SaveGames();
    SaveMain();
    ++revision;
}

void PlayerProfile::ScanRetroAchievements(const std::vector<std::pair<std::wstring, std::wstring>>& keyAndWorkspace,
                                          const std::map<std::wstring, std::pair<std::wstring, std::wstring>>& keySystemTitle) {
    std::vector<RaGame> found;
    for (const auto& [key, workspace] : keyAndWorkspace) {
        const std::string text = ReadBytes(workspace + L"\\retroachievements.txt");
        if (text.rfind("ranga-ra 1", 0) != 0) continue;
        RaGame game;
        game.key = key;
        if (const auto known = keySystemTitle.find(key); known != keySystemTitle.end()) game.system = known->second.first;
        for (const std::string& line : Lines(text)) {
            const auto parts = Split(line, '\t');
            if (parts[0] == "jogo" && parts.size() >= 4) {
                game.id = static_cast<uint32_t>(Number(parts[1]));
                game.badge = parts[2];
                game.title = parts[3];
            } else if (parts[0] == "c" && parts.size() >= 7) {
                RaAchievement achievement;
                achievement.id = static_cast<uint32_t>(Number(parts[1]));
                achievement.points = static_cast<int>(Number(parts[2]));
                achievement.unlocked = Number(parts[3]);
                achievement.badge = parts[4];
                achievement.title = parts[5];
                achievement.description = parts[6];
                game.achievements.push_back(achievement);
            }
        }
        if (game.id != 0) found.push_back(std::move(game));
    }
    if (found.size() != raGames.size()) ++revision;
    else for (size_t i = 0; i < found.size(); ++i) if (found[i].Unlocked() != raGames[i].Unlocked()) ++revision;
    raGames = std::move(found);
}

int64_t PlayerProfile::TotalSeconds() const {
    int64_t total = 0;
    for (const auto& [key, game] : games) total += game.seconds;
    return total;
}

int PlayerProfile::Consoles() const {
    std::set<std::wstring> systems;
    for (const auto& [key, game] : games) systems.insert(game.system);
    return static_cast<int>(systems.size());
}

int PlayerProfile::RangaUnlocked() const { return static_cast<int>(unlocked.size()); }

int PlayerProfile::RaUnlocked() const {
    int count = 0;
    for (const auto& game : raGames) count += game.Unlocked();
    return count;
}

int PlayerProfile::RaPoints() const {
    int points = 0;
    for (const auto& game : raGames) for (const auto& achievement : game.achievements) if (achievement.unlocked != 0) points += achievement.points;
    return points;
}

int PlayerProfile::Xp() const {
    // Ten XP an hour played, fifty for each of Ranga's achievements, and RetroAchievements' own points.
    return static_cast<int>(TotalSeconds() / 360) + 50 * RangaUnlocked() + RaPoints();
}

std::vector<std::string> PlayerProfile::Evaluate(int libraryCount) {
    int streak = 0, run = 0, previous = 0;
    for (const int day : days) {
        run = previous != 0 && NextDay(previous) == day ? run + 1 : 1;
        streak = std::max(streak, run);
        previous = day;
    }
    int64_t mostInOne = 0;
    for (const auto& [key, game] : games) mostInOne = std::max(mostInOne, game.seconds);
    int mastered = 0;
    for (const auto& game : raGames) mastered += !game.achievements.empty() && game.Unlocked() == static_cast<int>(game.achievements.size());
    std::vector<std::string> fresh;
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    for (size_t i = 0; i < achievements.size(); ++i) {
        const Definition& definition = kDefinitions[i];
        int progress = 0;
        switch (definition.kind) {
            case Kind::FirstGame: progress = static_cast<int>(games.size()); break;
            case Kind::TotalHours: progress = static_cast<int>(TotalSeconds() / 3600); break;
            case Kind::SessionHours: progress = static_cast<int>(longest / 3600); break;
            case Kind::Night: progress = nightOwl ? 1 : 0; break;
            case Kind::GameHours: progress = static_cast<int>(mostInOne / 3600); break;
            case Kind::Consoles: progress = Consoles(); break;
            case Kind::Library: progress = libraryCount; break;
            case Kind::GamesPlayed: progress = static_cast<int>(games.size()); break;
            case Kind::Streak: progress = streak; break;
            case Kind::Online: progress = static_cast<int>(online); break;
            case Kind::RaCount: progress = RaUnlocked(); break;
            case Kind::RaMastery: progress = mastered; break;
            case Kind::Photo: progress = avatar.empty() ? 0 : 1; break;
        }
        RangaAchievement& achievement = achievements[i];
        achievement.progress = std::min(progress, achievement.goal);
        if (achievement.unlocked == 0 && progress >= achievement.goal) {
            achievement.unlocked = now;
            unlocked[achievement.id] = now;
            fresh.push_back(achievement.title);
        }
    }
    if (!fresh.empty()) {
        SaveUnlocked();
        ++revision;
    }
    return fresh;
}

void PlayerProfile::SetName(const std::string& text) {
    name = Clean(text, 40);
    SaveMain();
    ++revision;
}

void PlayerProfile::SetPhrase(const std::string& text) {
    phrase = Clean(text, 120);
    SaveMain();
    ++revision;
}

void PlayerProfile::SetAvatar(const std::string& rgba) {
    avatar = rgba.size() == static_cast<size_t>(kAvatarSide) * kAvatarSide * 4 ? rgba : std::string();
    if (avatar.empty()) DeleteFileW((folder + L"\\avatar.rgba").c_str());
    else WriteWhole(folder + L"\\avatar.rgba", avatar);
    ++revision;
}

void PlayerProfile::SetRetroAchievements(const std::string& user, const std::string& token) {
    raUser = user;
    raToken = token;
    SaveMain();
}

PlayerCard PlayerProfile::Card(const std::string& playing, const std::string& playingSystem) const {
    PlayerCard card;
    card.id = id;
    card.name = name;
    card.phrase = phrase;
    card.seconds = TotalSeconds();
    card.xp = Xp();
    card.games = static_cast<int>(games.size());
    card.consoles = Consoles();
    card.achievements = RangaUnlocked() + RaUnlocked();
    card.raPoints = RaPoints();
    card.playing = playing;
    card.playingSystem = playingSystem;
    card.seen = static_cast<int64_t>(std::time(nullptr));
    for (const auto& [key, game] : games) card.recent.push_back(game);
    std::sort(card.recent.begin(), card.recent.end(), [](const PlayedGame& a, const PlayedGame& b) { return a.last > b.last; });
    if (card.recent.size() > 8) card.recent.resize(8);
    for (const auto& achievement : achievements) {
        if (achievement.unlocked != 0) card.showcase.push_back({false, achievement.unlocked, 50, achievement.id, achievement.title, achievement.description});
    }
    for (const auto& game : raGames) {
        for (const auto& achievement : game.achievements) {
            if (achievement.unlocked != 0) card.showcase.push_back({true, achievement.unlocked, achievement.points, achievement.badge, achievement.title, game.title + ": " + achievement.description});
        }
    }
    std::sort(card.showcase.begin(), card.showcase.end(), [](const ShowcaseItem& a, const ShowcaseItem& b) { return a.when > b.when; });
    if (card.showcase.size() > 8) card.showcase.resize(8);
    card.avatar = avatar;
    return card;
}

std::string WriteCard(const PlayerCard& card) {
    std::string text = "ranga-perfil 1\n";
    const auto put = [&](const std::string& line) { text += line + "\n"; };
    put("id\t" + Clean(card.id, 64));
    put("nome\t" + Clean(card.name, 40));
    put("frase\t" + Clean(card.phrase, 120));
    put("xp\t" + std::to_string(card.xp));
    put("segundos\t" + std::to_string(card.seconds));
    put("jogos\t" + std::to_string(card.games));
    put("consoles\t" + std::to_string(card.consoles));
    put("conquistas\t" + std::to_string(card.achievements));
    put("pontos_ra\t" + std::to_string(card.raPoints));
    if (card.closed) put("fechou\t1");
    if (!card.playing.empty()) put("jogando\t" + Clean(card.playing) + "\t" + Clean(card.playingSystem, 20));
    for (const auto& game : card.recent) {
        put("recente\t" + Clean(Utf8(game.system), 20) + "\t" + std::to_string(game.seconds) + "\t" + std::to_string(game.last) + "\t" + Clean(Utf8(game.title)));
    }
    for (const auto& item : card.showcase) {
        put("vitrine\t" + std::string(item.ra ? "1" : "0") + "\t" + std::to_string(item.when) + "\t" + std::to_string(item.points) + "\t" + Clean(item.badge, 64) +
            "\t" + Clean(item.title) + "\t" + Clean(item.description, 300));
    }
    if (!card.avatar.empty()) put("avatar\t" + Base64(card.avatar));
    return text;
}

std::vector<PlayerCard> ReadCards(const std::string& text) {
    std::vector<PlayerCard> cards;
    PlayerCard* card = nullptr;
    for (const std::string& line : Lines(text)) {
        if (line == "ranga-perfil 1") {
            cards.emplace_back();
            card = &cards.back();
            continue;
        }
        if (card == nullptr) continue;
        const auto parts = Split(line, '\t');
        const std::string& key = parts[0];
        const auto at = [&](size_t i) { return i < parts.size() ? parts[i] : std::string(); };
        if (key == "id") card->id = at(1);
        else if (key == "nome") card->name = at(1);
        else if (key == "frase") card->phrase = at(1);
        else if (key == "xp") card->xp = static_cast<int>(Number(at(1)));
        else if (key == "segundos") card->seconds = Number(at(1));
        else if (key == "jogos") card->games = static_cast<int>(Number(at(1)));
        else if (key == "consoles") card->consoles = static_cast<int>(Number(at(1)));
        else if (key == "conquistas") card->achievements = static_cast<int>(Number(at(1)));
        else if (key == "pontos_ra") card->raPoints = static_cast<int>(Number(at(1)));
        else if (key == "visto") card->seen = Number(at(1));
        else if (key == "fechou") card->closed = at(1) == "1";
        else if (key == "jogando") card->playing = at(1), card->playingSystem = at(2);
        else if (key == "recente" && parts.size() >= 5) {
            PlayedGame game;
            game.system = Wide(parts[1]);
            game.seconds = Number(parts[2]);
            game.last = Number(parts[3]);
            game.title = Wide(parts[4]);
            if (card->recent.size() < 8) card->recent.push_back(game);
        } else if (key == "vitrine" && parts.size() >= 7) {
            if (card->showcase.size() < 8) card->showcase.push_back({parts[1] == "1", Number(parts[2]), static_cast<int>(Number(parts[3])), parts[4], parts[5], parts[6]});
        } else if (key == "avatar") {
            std::string pixels = Unbase64(at(1));
            if (pixels.size() == static_cast<size_t>(kAvatarSide) * kAvatarSide * 4) card->avatar = std::move(pixels);
        }
    }
    cards.erase(std::remove_if(cards.begin(), cards.end(), [](const PlayerCard& c) { return c.id.empty(); }), cards.end());
    return cards;
}

std::string Base64(const std::string& bytes) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8) | static_cast<unsigned char>(bytes[i + 2]);
        out += table[v >> 18];
        out += table[(v >> 12) & 63];
        out += table[(v >> 6) & 63];
        out += table[v & 63];
    }
    if (i + 1 == bytes.size()) {
        const unsigned v = static_cast<unsigned char>(bytes[i]) << 16;
        out += table[v >> 18];
        out += table[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == bytes.size()) {
        const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8);
        out += table[v >> 18];
        out += table[(v >> 12) & 63];
        out += table[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

std::string Unbase64(const std::string& text) {
    std::string out;
    unsigned value = 0;
    int bits = 0;
    for (const char c : text) {
        int digit = -1;
        if (c >= 'A' && c <= 'Z') digit = c - 'A';
        else if (c >= 'a' && c <= 'z') digit = c - 'a' + 26;
        else if (c >= '0' && c <= '9') digit = c - '0' + 52;
        else if (c == '+') digit = 62;
        else if (c == '/') digit = 63;
        if (digit < 0) continue;
        value = (value << 6) | static_cast<unsigned>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((value >> bits) & 0xff);
        }
    }
    return out;
}

bool RetroAchievementsLogin(const std::string& user, const std::string& password, std::string& token, std::string& userOut, std::string& error) {
    const std::string body = "r=login2&u=" + UrlEncode(user) + "&p=" + UrlEncode(password);
    const HttpResult answer = HttpRequest("POST", "https://retroachievements.org/dorequest.php", body, "application/x-www-form-urlencoded", kRaUserAgent);
    if (answer.status == 0) {
        error = "Sem resposta do RetroAchievements. Confira a internet.";
        return false;
    }
    token = JsonString(answer.body, "Token");
    userOut = JsonString(answer.body, "User");
    if (answer.body.find("\"Success\":true") == std::string::npos || token.empty()) {
        const std::string code = JsonString(answer.body, "Code");
        error = JsonString(answer.body, "Error");
        if (code == "invalid_credentials") error = "Usuário ou senha errados.";
        else if (code == "access_denied" || code == "account_disabled") error = "O RetroAchievements bloqueou esta conta. Veja no site retroachievements.org.";
        if (error.empty()) error = "O RetroAchievements recusou (código " + std::to_string(answer.status) + ").";
        return false;
    }
    if (userOut.empty()) userOut = user;
    return true;
}

namespace {
std::atomic<unsigned long long> g_centralDownUntil{0};

std::string OnlyCodeCharacters(const std::string& text) {
    std::string out;
    for (const char c : text) if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') out += c;
    return out.substr(0, 64);
}

std::string OnlyAddressCharacters(const std::string& text) {
    std::string out;
    for (const char c : text) if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == ':' || c == '-' || c == '/') out += c;
    while (!out.empty() && out.back() == '/') out.pop_back();
    return out.substr(0, 128);
}

// The friends' central on Cloudflare (launcher/net/cloudflare), made on 18/11/2026; engines.ini [Ranga] central
// overrides it, and "nenhuma" leaves only the hosting PC's room.
constexpr const char* kDefaultCentral = "https://ranga-central.nadroi-dev.workers.dev";
}

std::string CentralServer(const std::wstring& root) {
    const std::string chosen = OnlyAddressCharacters(Utf8(ReadIni(root + L"\\engines.ini", L"Ranga", L"central", L"")));
    if (chosen == "nenhuma") return "";
    return chosen.empty() ? kDefaultCentral : chosen;
}

void SetCentralServer(const std::wstring& root, const std::string& address) {
    const std::wstring ini = root + L"\\engines.ini";
    EnsureUnicodeIni(ini);
    const std::string clean = OnlyAddressCharacters(address);
    WritePrivateProfileStringW(L"Ranga", L"central", clean.empty() ? nullptr : Wide(clean).c_str(), ini.c_str());
    g_centralDownUntil = 0;
}

std::string GroupCode(const std::wstring& root) { return OnlyCodeCharacters(Utf8(ReadIni(root + L"\\engines.ini", L"Ranga", L"grupo", L""))); }

void SetGroupCode(const std::wstring& root, const std::string& code) {
    const std::wstring ini = root + L"\\engines.ini";
    EnsureUnicodeIni(ini);
    const std::string clean = OnlyCodeCharacters(code);
    WritePrivateProfileStringW(L"Ranga", L"grupo", clean.empty() ? nullptr : Wide(clean).c_str(), ini.c_str());
}

SocialAnswer SocialRequest(const std::wstring& root, const std::string& method, const std::string& path, const std::string& body, int timeoutMs) {
    const std::string code = GroupCode(root);
    const std::string headers = code.empty() ? "" : "X-Ranga-Grupo: " + code + "\r\n";
    const std::string type = method == "POST" ? "text/plain; charset=utf-8" : "";
    const std::string central = CentralServer(root);
    if (!central.empty() && GetTickCount64() >= g_centralDownUntil) {
        HttpResult answer = HttpRequest(method, central + path, body, type, "RangaEmu", std::min(timeoutMs, 5000), headers);
        if (answer.status != 0) return {answer, central};
        // Down: the hosting PC's room for the next three minutes, without waiting on the central one each time.
        g_centralDownUntil = GetTickCount64() + 180000;
    }
    const std::string fallback = ProfilesServer(root);
    if (fallback == central) return {HttpResult{}, central};
    return {HttpRequest(method, fallback + path, body, type, "RangaEmu", timeoutMs, headers), fallback};
}

std::string ProfilesServer(const std::wstring& root) {
    const std::wstring chosen = ReadIni(root + L"\\engines.ini", L"Ranga", L"perfis", L"");
    if (!chosen.empty()) return Utf8(chosen);
    if (RoomServerEnabled(root)) return "127.0.0.1:" + std::to_string(ExtraPort(root, Extra::Classics));
    return Utf8(UpdateSource(root));
}

}
