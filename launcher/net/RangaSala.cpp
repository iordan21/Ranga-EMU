// tools\ranga-sala.exe: the table server of Ranga's online play for the consoles whose cores have none (RangaNet.hpp).
// It keeps no game: it seats everyone who runs the same game at one table, at most four, and passes each one's
// messages to the others. The first at a table runs the game for everyone (slot 0); when it leaves, the table ends.
//
//   ranga-sala.exe [--port 24873] [--log file] [--lan] [--atualizacao <pasta>] [--perfis <pasta>]
//
// With --atualizacao it also hands out Ranga's published update over HTTP on the same port (Update.hpp). With --perfis
// it keeps the players' profiles (PlayerProfile.hpp): POST /ranga/perfil stores one, GET /ranga/perfis hands all out.
//
// It runs on the PC that hosts, beside Eden's rooms; a TCP tunnel (playit) to 127.0.0.1 on its port lets friends in.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "RangaNet.hpp"

namespace {

FILE* g_log = nullptr;

void Log(const char* format, ...) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    for (FILE* out : {stdout, g_log}) {
        if (out == nullptr) continue;
        std::fprintf(out, "%02d/%02d %02d:%02d:%02d %s\n", now.wDay, now.wMonth, now.wHour, now.wMinute, now.wSecond, text);
        std::fflush(out);
    }
}

struct Player {
    SOCKET socket = INVALID_SOCKET;
    std::string address, game, name;
    int slot = -1;
    std::vector<uint8_t> in, out;
    size_t outAt = 0;
    bool closing = false;
    // A Ranga downloading an update: one HTTP request, the answer, and the line closes once it is sent.
    bool http = false, closeWhenSent = false;
};

// The published update (salas\atualizacao, made by the hosting Ranga): manifest.txt, manifest.sig and files\.
std::string g_updates;

void Answer(Player& player, int code, const std::string& body) {
    const std::string head = "HTTP/1.1 " + std::to_string(code) + (code == 200 ? " OK" : code == 404 ? " Not Found" : " Error") + "\r\nContent-Type: application/octet-stream\r\nContent-Length: " +
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

// The profiles: each one's card as its Ranga sent it, the secret that lets only that Ranga change it, and when.
struct StoredProfile {
    std::string secret, card;
    long long seen = 0;
};
std::string g_profilesFolder;
std::map<std::string, StoredProfile> g_profiles;
constexpr size_t kMaxProfiles = 200, kMaxProfileBody = 512 * 1024;

bool HexId(const std::string& text) {
    if (text.size() != 32) return false;
    for (const char c : text) if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

void LoadProfiles() {
    WIN32_FIND_DATAA found;
    HANDLE search = FindFirstFileA((g_profilesFolder + "\\*.txt").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        const std::string file = found.cFileName;
        if (file.size() != 36) continue;
        const std::string id = file.substr(0, 32);
        if (!HexId(id)) continue;
        const std::string text = ReadWhole(g_profilesFolder + "\\" + file);
        const size_t first = text.find('\n'), second = first == std::string::npos ? first : text.find('\n', first + 1);
        if (second == std::string::npos || text.rfind("segredo\t", 0) != 0 || text.compare(first + 1, 6, "visto\t") != 0) continue;
        StoredProfile& profile = g_profiles[id];
        profile.secret = text.substr(8, first - 8);
        profile.seen = std::atoll(text.c_str() + first + 7);
        profile.card = text.substr(second + 1);
    } while (FindNextFileA(search, &found));
    FindClose(search);
    Log("%zu perfis guardados", g_profiles.size());
}

// A card from a Ranga: "segredo<TAB>hex" then the card, whose "id" line names it. The first card of an id sets its
// secret; another Ranga cannot overwrite it without that secret.
int StoreProfile(const std::string& body) {
    if (g_profilesFolder.empty()) return 404;
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
    profile.seen = static_cast<long long>(std::time(nullptr));
    // Kept on disk, so the friends' profiles survive the server restarting.
    const std::string path = g_profilesFolder + "\\" + id + ".txt";
    if (FILE* file = std::fopen((path + ".novo").c_str(), "wb")) {
        const std::string head = "segredo\t" + secret + "\nvisto\t" + std::to_string(profile.seen) + "\n";
        std::fwrite(head.data(), 1, head.size(), file);
        std::fwrite(card.data(), 1, card.size(), file);
        std::fclose(file);
        MoveFileExA((path + ".novo").c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
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

// Only what the hosting Ranga publishes is served: the list, its signature and files\ by plain names (letters,
// digits, _ - . and one folder), never anything outside the update's folder.
void ServeHttp(Player& player) {
    const auto end = std::search(player.in.begin(), player.in.end(), "\r\n\r\n", "\r\n\r\n" + 4);
    if (end == player.in.end()) {
        if (player.in.size() > 8192) player.closing = true;
        return;
    }
    const std::string request(player.in.begin(), end);
    if (request.rfind("POST ", 0) == 0) {
        // A profile: the whole body has to be here first.
        std::string lower = request;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const size_t lengthAt = lower.find("content-length:");
        const size_t length = lengthAt == std::string::npos ? 0 : static_cast<size_t>(std::atoll(lower.c_str() + lengthAt + 15));
        if (length == 0 || length > kMaxProfileBody) {
            player.in.clear();
            Answer(player, 400, "");
            return;
        }
        const size_t start = static_cast<size_t>(end - player.in.begin()) + 4;
        if (player.in.size() < start + length) return;
        const std::string body(player.in.begin() + static_cast<std::ptrdiff_t>(start), player.in.begin() + static_cast<std::ptrdiff_t>(start + length));
        player.in.clear();
        const size_t space = request.find(' ', 5);
        const std::string path = space == std::string::npos ? "" : request.substr(5, space - 5);
        const int code = path == "/ranga/perfil" ? StoreProfile(body) : 404;
        Answer(player, code, "");
        if (code != 200) Log("perfil recusado (%d) de %s", code, player.address.c_str());
        return;
    }
    player.in.clear();
    const size_t space = request.find(' ', 4);
    const std::string path = space == std::string::npos ? "" : request.substr(4, space - 4);
    if (path == "/ranga/perfis" && !g_profilesFolder.empty()) {
        Answer(player, 200, AllProfiles());
        return;
    }
    bool safe = !g_updates.empty() && path.find("..") == std::string::npos;
    for (const char c : path) safe = safe && (std::isalnum(static_cast<unsigned char>(c)) || c == '/' || c == '_' || c == '-' || c == '.');
    std::string file;
    if (safe && path == "/ranga/manifest") file = g_updates + "\\manifest.txt";
    else if (safe && path == "/ranga/manifest.sig") file = g_updates + "\\manifest.sig";
    else if (safe && path.rfind("/ranga/f/", 0) == 0 && path.size() > 9 && std::count(path.begin() + 9, path.end(), '/') <= 1) {
        std::string relative = path.substr(9);
        std::replace(relative.begin(), relative.end(), '/', '\\');
        file = g_updates + "\\files\\" + relative;
    }
    const std::string body = file.empty() ? std::string() : ReadWhole(file);
    if (file.empty() || body.empty()) {
        Answer(player, 404, "");
        return;
    }
    Answer(player, 200, body);
    Log("atualização: %s -> %s (%zu bytes)", path.c_str(), player.address.c_str(), body.size());
}

std::vector<std::unique_ptr<Player>> g_players;
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

}

int main(int argc, char** argv) {
    int port = RangaNet::kDefaultPort;
    // Only this PC by default: the playit agent reaches it here, and Windows asks nothing about its firewall. --lan
    // opens it to the local network too.
    bool lan = false;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0) port = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--log") == 0) g_log = std::fopen(argv[++i], "a");
        else if (std::strcmp(argv[i], "--atualizacao") == 0) g_updates = argv[++i];
        else if (std::strcmp(argv[i], "--perfis") == 0) g_profilesFolder = argv[++i];
    }
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--lan") == 0) lan = true;
    }
    if (!g_profilesFolder.empty()) {
        CreateDirectoryA(g_profilesFolder.c_str(), nullptr);
        LoadProfiles();
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL exclusive = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in any{};
    any.sin_family = AF_INET;
    any.sin_port = htons(static_cast<u_short>(port));
    any.sin_addr.s_addr = htonl(lan ? INADDR_ANY : INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr*>(&any), sizeof(any)) != 0 || listen(listener, 16) != 0) {
        Log("a porta %d está em uso (erro %d)", port, WSAGetLastError());
        return 2;
    }
    u_long nonblocking = 1;
    ioctlsocket(listener, FIONBIO, &nonblocking);
    Log("sala do Ranga (jogos clássicos) aberta na porta TCP %d", port);

    std::vector<uint8_t> buffer(256 * 1024);
    for (;;) {
        fd_set reads, writes;
        FD_ZERO(&reads);
        FD_ZERO(&writes);
        FD_SET(listener, &reads);
        for (auto& player : g_players) {
            FD_SET(player->socket, &reads);
            if (player->outAt < player->out.size()) FD_SET(player->socket, &writes);
        }
        timeval wait{1, 0};
        if (select(0, &reads, &writes, nullptr, &wait) == SOCKET_ERROR) {
            Log("select falhou (erro %d)", WSAGetLastError());
            Sleep(100);
            continue;
        }
        if (FD_ISSET(listener, &reads)) {
            sockaddr_in from{};
            int fromSize = sizeof(from);
            SOCKET client = accept(listener, reinterpret_cast<sockaddr*>(&from), &fromSize);
            // select takes at most FD_SETSIZE sockets; past that a newcomer is turned away.
            if (client != INVALID_SOCKET && g_players.size() + 1 >= FD_SETSIZE) {
                closesocket(client);
            } else if (client != INVALID_SOCKET) {
                ioctlsocket(client, FIONBIO, &nonblocking);
                BOOL noDelay = TRUE;
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
                const int got = recv(player->socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
                if (got <= 0) {
                    if (got == 0 || WSAGetLastError() != WSAEWOULDBLOCK) player->closing = true;
                } else {
                    player->in.insert(player->in.end(), buffer.begin(), buffer.begin() + got);
                    // A Ranga asking for an update (HTTP) instead of sitting at a table.
                    if (player->slot < 0 && (player->http || (player->in.size() >= 4 && (std::memcmp(player->in.data(), "GET ", 4) == 0 || std::memcmp(player->in.data(), "POST", 4) == 0)))) {
                        player->http = true;
                        ServeHttp(*player);
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
                const int sent = send(player->socket, reinterpret_cast<const char*>(player->out.data() + player->outAt),
                                      static_cast<int>(std::min<size_t>(player->out.size() - player->outAt, 1u << 20)), 0);
                if (sent <= 0) {
                    if (WSAGetLastError() != WSAEWOULDBLOCK) player->closing = true;
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
            if (player.outAt < player.out.size()) send(player.socket, reinterpret_cast<const char*>(player.out.data() + player.outAt), static_cast<int>(player.out.size() - player.outAt), 0);
            closesocket(player.socket);
            g_players.erase(g_players.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}
