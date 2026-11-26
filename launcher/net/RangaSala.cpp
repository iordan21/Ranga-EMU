// tools\ranga-sala.exe (and ranga-sala on Linux, for an always-on server): the table server of Ranga's online play for
// the consoles whose cores have none (RangaNet.hpp). It keeps no game: it seats everyone who runs the same game at one
// table, at most four, and passes each one's messages to the others. The first at a table runs the game for everyone
// (slot 0); when it leaves, the table ends.
//
//   ranga-sala [--port 24873] [--log file] [--lan] [--atualizacao <pasta>] [--perfis <pasta>] [--grupo <codigo>]
//              [--hub-xbox <porta UDP>]
//
// --hub-xbox: the Xbox's System Link hub, on UDP. Each player's xemu (its udp network backend) sends the Xbox's
// Ethernet frames here and the hub passes every frame to all the others it heard from in the last minute, as a
// network switch would; the reply goes back to where each frame came from, so a playit UDP tunnel carries it.
//
// On the same port it answers plain HTTP too:
//   --atualizacao  hands out Ranga's published update (Update.hpp): GET /ranga/manifest, /ranga/manifest.sig, /ranga/f/...
//   --perfis       keeps the players' profiles (PlayerProfile.hpp) and the friends' chat, in that folder:
//                  POST /ranga/perfil, GET /ranga/perfis, POST /ranga/chat, GET /ranga/chat?desde=N
//   --grupo        the group's code: profiles and chat answer only a Ranga that sends it (X-Ranga-Grupo), so a stranger
//                  who finds the address reads and writes nothing. Updates stay open (they are signed anyway).
//
// On the PC that hosts it listens on 127.0.0.1, where a playit tunnel reaches it; --lan opens it to every network,
// which is what a server of its own (Oracle Cloud, launcher/net/oracle) needs.

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
using Socket = SOCKET;
#else
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
using Socket = int;
constexpr Socket INVALID_SOCKET = -1;
#endif

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <array>
#include <cctype>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "RangaNet.hpp"

namespace {

// ---- What differs between Windows and Linux ----

#ifdef _WIN32
constexpr char kSep = '\\';
void CloseSocket(Socket s) { closesocket(s); }
void NonBlocking(Socket s) {
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
}
bool WouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
int LastError() { return WSAGetLastError(); }
void MakeFolder(const std::string& path) { CreateDirectoryA(path.c_str(), nullptr); }
bool Replace(const std::string& from, const std::string& to) { return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0; }
void Pause(int ms) { Sleep(static_cast<DWORD>(ms)); }
std::vector<std::string> FilesIn(const std::string& folder) {
    std::vector<std::string> names;
    WIN32_FIND_DATAA found;
    HANDLE search = FindFirstFileA((folder + "\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return names;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) names.push_back(found.cFileName);
    } while (FindNextFileA(search, &found));
    FindClose(search);
    return names;
}
void LocalTime(time_t at, tm& out) { localtime_s(&out, &at); }
#else
constexpr char kSep = '/';
void CloseSocket(Socket s) { close(s); }
void NonBlocking(Socket s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
bool WouldBlock() { return errno == EWOULDBLOCK || errno == EAGAIN; }
int LastError() { return errno; }
void MakeFolder(const std::string& path) { mkdir(path.c_str(), 0755); }
bool Replace(const std::string& from, const std::string& to) { return rename(from.c_str(), to.c_str()) == 0; }
void Pause(int ms) { usleep(static_cast<useconds_t>(ms) * 1000); }
std::vector<std::string> FilesIn(const std::string& folder) {
    std::vector<std::string> names;
    if (DIR* dir = opendir(folder.c_str())) {
        while (dirent* entry = readdir(dir)) {
            if (entry->d_name[0] != '.') names.push_back(entry->d_name);
        }
        closedir(dir);
    }
    return names;
}
void LocalTime(time_t at, tm& out) { localtime_r(&at, &out); }
#endif

FILE* g_log = nullptr;

void Log(const char* format, ...) {
    tm now{};
    LocalTime(std::time(nullptr), now);
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    for (FILE* out : {stdout, g_log}) {
        if (out == nullptr) continue;
        std::fprintf(out, "%02d/%02d %02d:%02d:%02d %s\n", now.tm_mday, now.tm_mon + 1, now.tm_hour, now.tm_min, now.tm_sec, text);
        std::fflush(out);
    }
}

struct Player {
    Socket socket = INVALID_SOCKET;
    std::string address, game, name, title; // title: the game's name, when the Ranga sends it
    int slot = -1;
    std::vector<uint8_t> in, out;
    size_t outAt = 0;
    bool closing = false;
    // A Ranga asking over HTTP (update, profiles, chat): one request, the answer, and the line closes once it is sent.
    bool http = false, closeWhenSent = false;
};

// The published update (salas\atualizacao, made by the hosting Ranga): manifest.txt, manifest.sig and files\.
std::string g_updates;
// The group's code (--grupo); empty: profiles and chat are open, as on the hosting PC behind playit.
std::string g_group;
// --mesas-com-codigo: the tables too ask for the group's code (RangaNet.hpp kDenied).
bool g_tablesNeedGroup = false;

void Answer(Player& player, int code, const std::string& body) {
    const char* reason = code == 200 ? " OK" : code == 404 ? " Not Found" : code == 401 ? " Unauthorized" : code == 429 ? " Too Many Requests" : " Error";
    const std::string head = "HTTP/1.1 " + std::to_string(code) + reason + "\r\nContent-Type: application/octet-stream\r\nContent-Length: " +
                             std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    player.out.insert(player.out.end(), head.begin(), head.end());
    player.out.insert(player.out.end(), body.begin(), body.end());
    player.closeWhenSent = true;
}

std::string ReadWhole(const std::string& path) {
    std::string bytes;
    if (FILE* file = std::fopen(path.c_str(), "rb")) {
        char block[1 << 16];
        for (size_t got; (got = std::fread(block, 1, sizeof(block), file)) > 0;) bytes.append(block, got);
        std::fclose(file);
    }
    return bytes;
}

// Written beside and moved over, so a crash halfway never leaves half a file.
void WriteWhole(const std::string& path, const std::string& bytes) {
    if (FILE* file = std::fopen((path + ".novo").c_str(), "wb")) {
        const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
        std::fclose(file);
        if (ok) Replace(path + ".novo", path);
    }
}

// ---- Profiles ----

// Each one's card as its Ranga sent it, the secret that lets only that Ranga change it, and when.
struct StoredProfile {
    std::string secret, card, name;
    long long seen = 0;
};
std::string g_dataFolder;
// Switch shader caches to hand out (--shaders): <title id>.bin each.
std::string g_shaders;
std::map<std::string, StoredProfile> g_profiles;
constexpr size_t kMaxProfiles = 200, kMaxBody = 512 * 1024;

bool HexId(const std::string& text) {
    if (text.size() != 32) return false;
    for (const char c : text) if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

// The value of a "key<TAB>value" line.
std::string LineValue(const std::string& text, const std::string& key) {
    const std::string start = key + "\t";
    size_t at = 0;
    if (text.rfind(start, 0) != 0) {
        at = text.find("\n" + start);
        if (at == std::string::npos) return "";
        at += 1;
    }
    at += start.size();
    const size_t end = text.find('\n', at);
    return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

void LoadProfiles() {
    for (const std::string& file : FilesIn(g_dataFolder)) {
        if (file.size() != 36 || file.compare(32, 4, ".txt") != 0) continue;
        const std::string id = file.substr(0, 32);
        if (!HexId(id)) continue;
        const std::string text = ReadWhole(g_dataFolder + kSep + file);
        const size_t first = text.find('\n'), second = first == std::string::npos ? first : text.find('\n', first + 1);
        if (second == std::string::npos || text.rfind("segredo\t", 0) != 0 || text.compare(first + 1, 6, "visto\t") != 0) continue;
        StoredProfile& profile = g_profiles[id];
        profile.secret = text.substr(8, first - 8);
        profile.seen = std::atoll(text.c_str() + first + 7);
        profile.card = text.substr(second + 1);
        profile.name = LineValue(profile.card, "nome");
    }
    Log("%zu perfis guardados", g_profiles.size());
}

// A card from a Ranga: "segredo<TAB>hex" then the card, whose "id" line names it. The first card of an id sets its
// secret; another Ranga cannot overwrite it without that secret.
int StoreProfile(const std::string& body) {
    if (g_dataFolder.empty()) return 404;
    const size_t first = body.find('\n');
    if (body.rfind("segredo\t", 0) != 0 || first == std::string::npos) return 400;
    const std::string secret = body.substr(8, first - 8);
    const std::string card = body.substr(first + 1);
    const size_t idAt = card.find("\nid\t");
    if (card.rfind("ranga-perfil 1\n", 0) != 0 || idAt == std::string::npos || !HexId(secret)) return 400;
    const std::string id = card.substr(idAt + 4, 32);
    if (!HexId(id)) return 400;
    const auto known = g_profiles.find(id);
    if (known != g_profiles.end() && known->second.secret != secret) return 403;
    if (known == g_profiles.end() && g_profiles.size() >= kMaxProfiles) return 507;
    StoredProfile& profile = g_profiles[id];
    profile.secret = secret;
    profile.card = card;
    profile.name = LineValue(card, "nome");
    profile.seen = static_cast<long long>(std::time(nullptr));
    // Kept on disk, so the friends' profiles survive the server restarting.
    WriteWhole(g_dataFolder + kSep + id + ".txt", "segredo\t" + secret + "\nvisto\t" + std::to_string(profile.seen) + "\n" + card);
    return 200;
}

// Every card, each with when it was last sent ("visto"), never a secret.
std::string AllProfiles() {
    std::string out;
    for (const auto& [id, profile] : g_profiles) {
        std::string card = profile.card;
        if (!card.empty() && card.back() != '\n') card += '\n';
        out += card + "visto\t" + std::to_string(profile.seen) + "\n";
    }
    return out;
}

// ---- Chat ----

// The friends' messages: the last kKeptMessages, numbered, in chat.txt beside the profiles.
struct Message {
    long long number = 0, when = 0;
    std::string id, name, text;
};
std::deque<Message> g_chat;
long long g_chatNumber = 0;
std::map<std::string, long long> g_lastSaid;
constexpr size_t kKeptMessages = 500, kMaxText = 500;

std::string ChatLine(const Message& m) {
    return "m\t" + std::to_string(m.number) + "\t" + std::to_string(m.when) + "\t" + m.id + "\t" + m.name + "\t" + m.text + "\n";
}

void SaveChat() {
    std::string text;
    for (const Message& m : g_chat) text += ChatLine(m);
    WriteWhole(g_dataFolder + kSep + "chat.txt", text);
}

void LoadChat() {
    const std::string text = ReadWhole(g_dataFolder + kSep + "chat.txt");
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        std::vector<std::string> parts;
        for (size_t from = 0;;) {
            const size_t tab = line.find('\t', from);
            parts.push_back(line.substr(from, tab == std::string::npos ? std::string::npos : tab - from));
            if (tab == std::string::npos) break;
            from = tab + 1;
        }
        if (parts.size() != 6 || parts[0] != "m") continue;
        Message m{std::atoll(parts[1].c_str()), std::atoll(parts[2].c_str()), parts[3], parts[4], parts[5]};
        g_chat.push_back(m);
        g_chatNumber = std::max(g_chatNumber, m.number);
    }
    while (g_chat.size() > kKeptMessages) g_chat.pop_front();
}

std::string Clean(std::string text, size_t limit) {
    for (char& c : text) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    if (text.size() > limit) {
        text.resize(limit);
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xc0) == 0x80) text.pop_back();
        if (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xc0) == 0xc0) text.pop_back();
    }
    return text;
}

// A message: "segredo", "id" and "texto" lines. Only a profile's own Ranga speaks as it, the name comes from the
// profile, and one message a second at most.
int Say(const std::string& body) {
    if (g_dataFolder.empty()) return 404;
    const std::string secret = LineValue(body, "segredo"), id = LineValue(body, "id");
    std::string text = Clean(LineValue(body, "texto"), kMaxText);
    const auto profile = g_profiles.find(id);
    if (profile == g_profiles.end() || profile->second.secret != secret) return 403;
    if (text.find_first_not_of(' ') == std::string::npos) return 400;
    const long long now = static_cast<long long>(std::time(nullptr));
    long long& last = g_lastSaid[id];
    if (now - last < 1) return 429;
    last = now;
    g_chat.push_back({++g_chatNumber, now, id, Clean(profile->second.name, 40), text});
    while (g_chat.size() > kKeptMessages) g_chat.pop_front();
    SaveChat();
    return 200;
}

std::string ChatSince(long long after) {
    std::string out;
    for (const Message& m : g_chat) if (m.number > after) out += ChatLine(m);
    return out;
}

// ---- HTTP ----

std::string Header(const std::string& request, const std::string& name) {
    std::string lower = request;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const size_t at = lower.find("\r\n" + name + ":");
    if (at == std::string::npos) return "";
    size_t from = at + 3 + name.size();
    while (from < request.size() && request[from] == ' ') ++from;
    const size_t end = request.find("\r\n", from);
    return request.substr(from, end == std::string::npos ? std::string::npos : end - from);
}

std::string TablesText();

void ServeHttp(Player& player) {
    const auto end = std::search(player.in.begin(), player.in.end(), "\r\n\r\n", "\r\n\r\n" + 4);
    if (end == player.in.end()) {
        if (player.in.size() > 8192) player.closing = true;
        return;
    }
    const std::string request(player.in.begin(), end);
    const bool post = request.rfind("POST ", 0) == 0;
    const size_t pathAt = post ? 5 : 4, space = request.find(' ', pathAt);
    std::string path = space == std::string::npos ? "" : request.substr(pathAt, space - pathAt), query;
    if (const size_t mark = path.find('?'); mark != std::string::npos) {
        query = path.substr(mark + 1);
        path.resize(mark);
    }
    std::string body;
    if (post) {
        // The whole body has to be here first.
        const size_t length = static_cast<size_t>(std::atoll(Header(request, "content-length").c_str()));
        if (length == 0 || length > kMaxBody) {
            player.in.clear();
            Answer(player, 400, "");
            return;
        }
        const size_t start = static_cast<size_t>(end - player.in.begin()) + 4;
        if (player.in.size() < start + length) return;
        body.assign(player.in.begin() + static_cast<std::ptrdiff_t>(start), player.in.begin() + static_cast<std::ptrdiff_t>(start + length));
    }
    player.in.clear();

    // (/ranga/mesas stays open: anyone with the address can sit at a table anyway, so the names it lists are no secret.)
    const bool social = path == "/ranga/perfil" || path == "/ranga/perfis" || path == "/ranga/chat";
    if (social && !g_group.empty() && Header(request, "x-ranga-grupo") != g_group) {
        Answer(player, 401, "");
        return;
    }
    if (post) {
        const int code = path == "/ranga/perfil" ? StoreProfile(body) : path == "/ranga/chat" ? Say(body) : 404;
        Answer(player, code, "");
        if (code != 200 && code != 429) Log("%s recusado (%d) de %s", path.c_str(), code, player.address.c_str());
        return;
    }
    // Who plays what at the tables now: one line each, "game's name<TAB>player's name".
    if (path == "/ranga/mesas") {
        Answer(player, 200, TablesText());
        return;
    }
    if (path == "/ranga/perfis" && !g_dataFolder.empty()) {
        Answer(player, 200, AllProfiles());
        return;
    }
    if (path == "/ranga/chat" && !g_dataFolder.empty()) {
        const long long after = query.rfind("desde=", 0) == 0 ? std::atoll(query.c_str() + 6) : 0;
        Answer(player, 200, ChatSince(after));
        return;
    }
    // The update: only what the hosting Ranga published, by plain names (letters, digits, _ - . and one folder).
    bool safe = !g_updates.empty() && path.find("..") == std::string::npos;
    for (const char c : path) safe = safe && (std::isalnum(static_cast<unsigned char>(c)) || c == '/' || c == '_' || c == '-' || c == '.');
    std::string file;
    // A Switch game's shader cache (Eden's vulkan.bin, the part that works on any GPU), by its 16-digit title id:
    // the hosting Ranga keeps its own in --shaders, and friends fetch it so their first races don't stutter.
    if (path.rfind("/ranga/shaders/", 0) == 0 && !g_shaders.empty()) {
        const std::string id = path.substr(15);
        const bool valid = id.size() == 20 && id.compare(16, 4, ".bin") == 0 &&
                           std::all_of(id.begin(), id.begin() + 16, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
        const std::string bytes = valid ? ReadWhole(g_shaders + kSep + id) : std::string();
        Answer(player, bytes.empty() ? 404 : 200, bytes);
        if (!bytes.empty()) Log("cache de shaders %s -> %s (%zu bytes)", id.c_str(), player.address.c_str(), bytes.size());
        return;
    }
    if (safe && path == "/ranga/manifest") file = g_updates + kSep + "manifest.txt";
    else if (safe && path == "/ranga/manifest.sig") file = g_updates + kSep + "manifest.sig";
    else if (safe && path.rfind("/ranga/f/", 0) == 0 && path.size() > 9 && std::count(path.begin() + 9, path.end(), '/') <= 1) {
        std::string relative = path.substr(9);
        std::replace(relative.begin(), relative.end(), '/', kSep);
        file = g_updates + kSep + "files" + kSep + relative;
    }
    const std::string bytes = file.empty() ? std::string() : ReadWhole(file);
    if (file.empty() || bytes.empty()) {
        Answer(player, 404, "");
        return;
    }
    Answer(player, 200, bytes);
    Log("atualização: %s -> %s (%zu bytes)", path.c_str(), player.address.c_str(), bytes.size());
}

// ---- Tables ----

std::vector<std::unique_ptr<Player>> g_players;
std::string TablesText() {
    std::string text;
    for (const auto& seated : g_players) {
        if (seated->slot < 0 || seated->closing) continue;
        text += (seated->title.empty() ? "um jogo" : seated->title) + "\t" + seated->name + "\n";
    }
    return text;
}

// game -> who sits at each slot
std::map<std::string, std::array<Player*, RangaNet::kMaxPlayers>> g_tables;

// A player who reads too slowly to keep up (more than this waiting) is let go rather than kept growing.
constexpr size_t kMaxQueued = 256u << 20;

void Send(Player& player, const std::vector<uint8_t>& bytes) {
    if (player.closing) return;
    if (player.out.size() - player.outAt + bytes.size() > kMaxQueued) {
        Log("%s (%s) não acompanha; saiu", player.name.c_str(), player.address.c_str());
        player.closing = true;
        return;
    }
    player.out.insert(player.out.end(), bytes.begin(), bytes.end());
}

Player** TableOf(const Player& player) {
    const auto found = g_tables.find(player.game);
    return found == g_tables.end() ? nullptr : found->second.data();
}

void Seat(Player& player, uint8_t version, const std::string& game, const std::string& name) {
    if (version != RangaNet::kVersion) {
        Log("%s: versão %u do Ranga, este servidor é da %u", player.address.c_str(), version, RangaNet::kVersion);
        player.closing = true;
        return;
    }
    player.game = game;
    player.name = name.empty() ? "Jogador" : name;
    auto& table = g_tables[game]; // a new table comes with every seat empty
    int slot = -1;
    for (int i = 0; i < RangaNet::kMaxPlayers && slot < 0; ++i) if (table[i] == nullptr) slot = i;
    if (slot < 0) {
        Send(player, RangaNet::Writer(RangaNet::kFull).Done());
        player.closing = true;
        Log("%s: a mesa de %s está cheia", player.name.c_str(), game.c_str());
        return;
    }
    // Slot 0 runs the game for the table; a table whose first seat is empty has no game running and takes the
    // newcomer there.
    table[slot] = &player;
    player.slot = slot;
    RangaNet::Writer welcome(RangaNet::kWelcome);
    welcome.U8(static_cast<uint8_t>(slot));
    uint8_t count = 0;
    for (auto* seat : table) count += seat != nullptr && seat != &player;
    welcome.U8(count);
    for (int i = 0; i < RangaNet::kMaxPlayers; ++i) {
        if (table[i] != nullptr && table[i] != &player) welcome.U8(static_cast<uint8_t>(i)).Str(table[i]->name);
    }
    Send(player, welcome.Done());
    auto join = RangaNet::Writer(RangaNet::kJoin).U8(static_cast<uint8_t>(slot)).Str(player.name).Done();
    for (auto* seat : table) if (seat != nullptr && seat != &player) Send(*seat, join);
    Log("%s (%s) sentou no lugar %d de %s", player.name.c_str(), player.address.c_str(), slot + 1, game.c_str());
}

void Unseat(Player& player) {
    Player** table = TableOf(player);
    if (table == nullptr || player.slot < 0 || table[player.slot] != &player) return;
    table[player.slot] = nullptr;
    auto leave = RangaNet::Writer(RangaNet::kLeave).U8(static_cast<uint8_t>(player.slot)).Done();
    bool anyone = false;
    for (int i = 0; i < RangaNet::kMaxPlayers; ++i) {
        if (table[i] == nullptr) continue;
        anyone = true;
        Send(*table[i], leave);
        // The game ran on the one who left: everyone else is let go and comes back to a fresh table.
        if (player.slot == 0) table[i]->closing = true;
    }
    Log("%s saiu de %s", player.name.c_str(), player.game.c_str());
    if (!anyone || player.slot == 0) {
        for (int i = 0; i < RangaNet::kMaxPlayers; ++i) if (table[i] != nullptr) table[i]->slot = -1;
        g_tables.erase(player.game);
    }
    player.slot = -1;
}

void Handle(Player& player, uint8_t type, const uint8_t* body, size_t size) {
    if (type == RangaNet::kHello && player.slot < 0) {
        RangaNet::Reader read(body, size);
        const uint8_t version = read.U8();
        const std::string game = read.Str(), name = read.Str();
        if (!read.Ok() || game.empty()) { player.closing = true; return; }
        // A Ranga since 1.0.1 sends the game's name too, for /ranga/mesas; one from before ends here.
        const std::string title = read.Left() > 0 ? read.Str() : std::string();
        // The group's code (RangaNet.hpp): with --mesas-com-codigo, only who brings it sits down. Off until every
        // friend runs a Ranga that has the code (it comes from the central with the rooms' addresses, since 1.8.2).
        const std::string group = read.Left() > 0 ? read.Str() : std::string();
        if (g_tablesNeedGroup && !g_group.empty() && (group.size() != g_group.size() || !std::equal(group.begin(), group.end(), g_group.begin()))) {
            Send(player, RangaNet::Writer(RangaNet::kDenied).Done());
            player.closeWhenSent = true;
            Log("%s recusado na mesa: sem o código do grupo", player.address.c_str());
            return;
        }
        player.title = title.size() > 120 ? title.substr(0, 120) : title;
        Seat(player, version, game, name);
        return;
    }
    if (type < RangaNet::kRelay || player.slot < 0) return;
    Player** table = TableOf(player);
    if (table == nullptr) return;
    std::vector<uint8_t> relayed(4 + 2 + size);
    const uint32_t length = static_cast<uint32_t>(2 + size);
    std::memcpy(relayed.data(), &length, 4);
    relayed[4] = type;
    relayed[5] = static_cast<uint8_t>(player.slot);
    if (size != 0) std::memcpy(relayed.data() + 6, body, size);
    for (int i = 0; i < RangaNet::kMaxPlayers; ++i) if (table[i] != nullptr && table[i] != &player) Send(*table[i], relayed);
}

bool StartsHttp(const std::vector<uint8_t>& in) {
    return in.size() >= 4 && (std::memcmp(in.data(), "GET ", 4) == 0 || std::memcmp(in.data(), "POST", 4) == 0);
}

}

int main(int argc, char** argv) {
    int port = RangaNet::kDefaultPort, hubPort = 0;
    bool lan = false;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0) port = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--log") == 0) g_log = std::fopen(argv[++i], "a");
        else if (std::strcmp(argv[i], "--atualizacao") == 0) g_updates = argv[++i];
        else if (std::strcmp(argv[i], "--shaders") == 0) g_shaders = argv[++i];
        else if (std::strcmp(argv[i], "--perfis") == 0) g_dataFolder = argv[++i];
        else if (std::strcmp(argv[i], "--grupo") == 0) g_group = argv[++i];
        else if (std::strcmp(argv[i], "--hub-xbox") == 0) hubPort = std::atoi(argv[++i]);
    }
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--lan") == 0) lan = true;
        if (std::strcmp(argv[i], "--mesas-com-codigo") == 0) g_tablesNeedGroup = true;
    }
    if (!g_dataFolder.empty()) {
        MakeFolder(g_dataFolder);
        LoadProfiles();
        LoadChat();
    }
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
#else
    // A friend whose line drops while an answer is going is no reason for the server to end.
    signal(SIGPIPE, SIG_IGN);
#endif
    Socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
    BOOL exclusive = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    sockaddr_in any{};
    any.sin_family = AF_INET;
    any.sin_port = htons(static_cast<uint16_t>(port));
    any.sin_addr.s_addr = htonl(lan ? INADDR_ANY : INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr*>(&any), sizeof(any)) != 0 || listen(listener, 16) != 0) {
        Log("a porta %d está em uso (erro %d)", port, LastError());
        return 2;
    }
    NonBlocking(listener);
    Log("sala do Ranga (jogos clássicos) aberta na porta TCP %d%s%s", port, lan ? ", para todas as redes" : "", g_group.empty() ? "" : ", com código do grupo");

    // The Xbox hub: who sent frames lately (address and port as seen), and when.
    Socket hub = INVALID_SOCKET;
    struct HubPeer {
        sockaddr_in address;
        time_t seen;
    };
    std::vector<HubPeer> hubPeers;
    if (hubPort > 0 && hubPort < 65536) {
        hub = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in hubAddress{};
        hubAddress.sin_family = AF_INET;
        hubAddress.sin_port = htons(static_cast<uint16_t>(hubPort));
        hubAddress.sin_addr.s_addr = htonl(lan ? INADDR_ANY : INADDR_LOOPBACK);
        if (hub == INVALID_SOCKET || bind(hub, reinterpret_cast<sockaddr*>(&hubAddress), sizeof(hubAddress)) != 0) {
            Log("a porta UDP %d do hub do Xbox está em uso (erro %d); sigo sem ele", hubPort, LastError());
            if (hub != INVALID_SOCKET) CloseSocket(hub);
            hub = INVALID_SOCKET;
        } else {
            NonBlocking(hub);
            Log("hub do Xbox (System Link) aberto na porta UDP %d", hubPort);
        }
    }

    std::vector<uint8_t> buffer(256 * 1024);
    for (;;) {
        fd_set reads, writes;
        FD_ZERO(&reads);
        FD_ZERO(&writes);
        FD_SET(listener, &reads);
        Socket highest = listener;
        if (hub != INVALID_SOCKET) {
            FD_SET(hub, &reads);
            highest = std::max(highest, hub);
        }
        for (auto& player : g_players) {
            FD_SET(player->socket, &reads);
            if (player->outAt < player->out.size()) FD_SET(player->socket, &writes);
            highest = std::max(highest, player->socket);
        }
        timeval wait{1, 0};
        if (select(static_cast<int>(highest + 1), &reads, &writes, nullptr, &wait) < 0) {
            Log("select falhou (erro %d)", LastError());
            Pause(100);
            continue;
        }
        if (hub != INVALID_SOCKET && FD_ISSET(hub, &reads)) {
            for (;;) {
                sockaddr_in from{};
                socklen_t fromSize = sizeof(from);
                const int got = static_cast<int>(recvfrom(hub, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0,
                                                          reinterpret_cast<sockaddr*>(&from), &fromSize));
                if (got <= 0) break;
                const time_t now = std::time(nullptr);
                bool known = false;
                for (auto& peer : hubPeers) {
                    if (peer.address.sin_addr.s_addr == from.sin_addr.s_addr && peer.address.sin_port == from.sin_port) {
                        peer.seen = now;
                        known = true;
                    }
                }
                if (!known && hubPeers.size() < 16) {
                    hubPeers.push_back({from, now});
                    char text[64] = {};
                    inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text));
                    Log("hub do Xbox: entrou %s:%d (%zu no hub)", text, ntohs(from.sin_port), hubPeers.size());
                }
                hubPeers.erase(std::remove_if(hubPeers.begin(), hubPeers.end(), [&](const HubPeer& peer) { return now - peer.seen > 60; }),
                               hubPeers.end());
                for (const auto& peer : hubPeers) {
                    if (peer.address.sin_addr.s_addr == from.sin_addr.s_addr && peer.address.sin_port == from.sin_port) continue;
                    sendto(hub, reinterpret_cast<const char*>(buffer.data()), got, 0, reinterpret_cast<const sockaddr*>(&peer.address), sizeof(peer.address));
                }
            }
        }
        if (FD_ISSET(listener, &reads)) {
            sockaddr_in from{};
            socklen_t fromSize = sizeof(from);
            Socket client = accept(listener, reinterpret_cast<sockaddr*>(&from), &fromSize);
            // select takes at most FD_SETSIZE sockets; past that a newcomer is turned away.
            if (client != INVALID_SOCKET && (g_players.size() + 1 >= FD_SETSIZE
#ifndef _WIN32
                                             || client >= FD_SETSIZE
#endif
                                             )) {
                CloseSocket(client);
            } else if (client != INVALID_SOCKET) {
                NonBlocking(client);
                int noDelay = 1;
                setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
                auto player = std::make_unique<Player>();
                player->socket = client;
                char text[64] = {};
                inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text));
                player->address = std::string(text) + ":" + std::to_string(ntohs(from.sin_port));
                g_players.push_back(std::move(player));
            }
        }
        for (auto& player : g_players) {
            if (player->closing) continue;
            if (FD_ISSET(player->socket, &reads)) {
                const int got = static_cast<int>(recv(player->socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0));
                if (got <= 0) {
                    if (got == 0 || !WouldBlock()) player->closing = true;
                } else {
                    player->in.insert(player->in.end(), buffer.begin(), buffer.begin() + got);
                    // A Ranga asking over HTTP instead of sitting at a table.
                    if (player->slot < 0 && (player->http || StartsHttp(player->in))) {
                        player->http = true;
                        ServeHttp(*player);
                        continue;
                    }
                    // Before sitting down, only a Hello's worth: whoever knows the address cannot fill this PC's memory.
                    if (player->slot < 0 && player->in.size() > RangaNet::kMaxBeforeSeat) {
                        Log("%s mandou demais antes de sentar", player->address.c_str());
                        player->closing = true;
                        continue;
                    }
                    if (!RangaNet::TakeMessages(player->in, [&](uint8_t type, const uint8_t* body, size_t size) { Handle(*player, type, body, size); })) {
                        Log("%s mandou dados que não são do Ranga", player->address.c_str());
                        player->closing = true;
                    }
                }
            }
        }
        for (auto& player : g_players) {
            while (player->outAt < player->out.size()) {
                const int sent = static_cast<int>(send(player->socket, reinterpret_cast<const char*>(player->out.data() + player->outAt),
                                                       static_cast<int>(std::min<size_t>(player->out.size() - player->outAt, 1u << 20)), 0));
                if (sent <= 0) {
                    if (!WouldBlock()) player->closing = true;
                    break;
                }
                player->outAt += static_cast<size_t>(sent);
            }
            if (player->outAt == player->out.size()) {
                player->out.clear();
                player->outAt = 0;
                if (player->closeWhenSent) player->closing = true;
            } else if (player->outAt > (8u << 20)) {
                player->out.erase(player->out.begin(), player->out.begin() + static_cast<std::ptrdiff_t>(player->outAt));
                player->outAt = 0;
            }
        }
        // Those leaving go last, so what was queued for the others on their way out still goes.
        for (size_t i = 0; i < g_players.size();) {
            Player& player = *g_players[i];
            if (!player.closing) { ++i; continue; }
            Unseat(player);
            if (player.outAt < player.out.size())
                send(player.socket, reinterpret_cast<const char*>(player.out.data() + player.outAt), static_cast<int>(player.out.size() - player.outAt), 0);
            CloseSocket(player.socket);
            g_players.erase(g_players.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}
