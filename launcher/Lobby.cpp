#include "Lobby.hpp"
#include "Http.hpp"

#ifndef UNICODE
#define UNICODE
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>

#include <cctype>
#include <cstring>
#include <cwctype>
#include <cstdlib>
#include <memory>

namespace Ranga {

namespace {

// Eden's room protocol version (eden/src/network/room.h network_version): a room of another version refuses it.
constexpr int kNetworkVersion = 1;
constexpr const char* kLobby = "https://api.ynet-fun.xyz/lobby";

// A small JSON reader, enough for the lobby: numbers are kept as their text, so 64-bit game ids stay exact.
struct Json {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool boolean = false;
    std::string text; // a string's value or a number's digits
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;

    const Json* Get(const char* key) const {
        for (const auto& [name, value] : fields) if (name == key) return &value;
        return nullptr;
    }
    std::string Str(const char* key) const {
        const Json* value = Get(key);
        return value != nullptr && value->kind == String ? value->text : "";
    }
    long long Int(const char* key) const {
        const Json* value = Get(key);
        return value != nullptr && value->kind == Number ? std::atoll(value->text.c_str()) : 0;
    }
    uint64_t Id(const char* key) const {
        const Json* value = Get(key);
        return value != nullptr && value->kind == Number ? std::strtoull(value->text.c_str(), nullptr, 10) : 0;
    }
    bool Flag(const char* key) const {
        const Json* value = Get(key);
        return value != nullptr && value->kind == Bool && value->boolean;
    }
};

class Parser {
public:
    explicit Parser(const std::string& source) : s(source) {}
    bool Parse(Json& out) {
        if (!Value(out, 0)) return false;
        Space();
        return at == s.size();
    }

private:
    void Space() {
        while (at < s.size() && std::isspace(static_cast<unsigned char>(s[at]))) ++at;
    }
    bool Literal(const char* word) {
        const size_t size = std::strlen(word);
        if (s.compare(at, size, word) != 0) return false;
        at += size;
        return true;
    }
    static void PutUtf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xc0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3f));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xe0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (code & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (code & 0x3f));
        }
    }
    bool Hex4(unsigned& code) {
        if (at + 4 > s.size()) return false;
        code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s[at++];
            code <<= 4;
            if (c >= '0' && c <= '9') code |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') code |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') code |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool StringValue(std::string& out) {
        if (at >= s.size() || s[at] != '"') return false;
        ++at;
        while (at < s.size()) {
            const char c = s[at++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at >= s.size()) return false;
            const char e = s[at++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    unsigned code = 0;
                    if (!Hex4(code)) return false;
                    // A surrogate pair is one character (the emoji in room names).
                    if (code >= 0xd800 && code < 0xdc00 && at + 6 <= s.size() && s[at] == '\\' && s[at + 1] == 'u') {
                        at += 2;
                        unsigned low = 0;
                        if (!Hex4(low)) return false;
                        code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                    }
                    PutUtf8(out, code);
                    break;
                }
                default: out += e;
            }
        }
        return false;
    }
    bool Value(Json& out, int depth) {
        if (depth > 32) return false;
        Space();
        if (at >= s.size()) return false;
        const char c = s[at];
        if (c == '{') {
            out.kind = Json::Object;
            ++at;
            Space();
            if (at < s.size() && s[at] == '}') { ++at; return true; }
            for (;;) {
                Space();
                std::string key;
                if (!StringValue(key)) return false;
                Space();
                if (at >= s.size() || s[at++] != ':') return false;
                Json value;
                if (!Value(value, depth + 1)) return false;
                out.fields.emplace_back(std::move(key), std::move(value));
                Space();
                if (at < s.size() && s[at] == ',') { ++at; continue; }
                if (at < s.size() && s[at] == '}') { ++at; return true; }
                return false;
            }
        }
        if (c == '[') {
            out.kind = Json::Array;
            ++at;
            Space();
            if (at < s.size() && s[at] == ']') { ++at; return true; }
            for (;;) {
                Json value;
                if (!Value(value, depth + 1)) return false;
                out.items.push_back(std::move(value));
                Space();
                if (at < s.size() && s[at] == ',') { ++at; continue; }
                if (at < s.size() && s[at] == ']') { ++at; return true; }
                return false;
            }
        }
        if (c == '"') {
            out.kind = Json::String;
            return StringValue(out.text);
        }
        if (Literal("true")) { out.kind = Json::Bool; out.boolean = true; return true; }
        if (Literal("false")) { out.kind = Json::Bool; return true; }
        if (Literal("null")) return true;
        const size_t start = at;
        while (at < s.size() && (std::isdigit(static_cast<unsigned char>(s[at])) || s[at] == '-' || s[at] == '+' || s[at] == '.' || s[at] == 'e' || s[at] == 'E')) ++at;
        if (at == start) return false;
        out.kind = Json::Number;
        out.text = s.substr(start, at - start);
        return true;
    }

    const std::string& s;
    size_t at = 0;
};

// Room names are full of emoji and "fancy" letters (outside the basic plane), which Ranga's font does not have: they
// go, with the joiners and variation selectors around them, and the spaces left are made single.
std::string Readable(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const size_t length = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        const std::string piece = text.substr(i, length);
        i += length;
        if (length == 4) continue;
        if (length == 3 && piece.size() == 3) {
            const unsigned code = ((c & 0x0fu) << 12) | ((static_cast<unsigned char>(piece[1]) & 0x3fu) << 6) | (static_cast<unsigned char>(piece[2]) & 0x3fu);
            // Arrows, symbols, dingbats and box drawing (U+2190-2BFF), private use, variation selectors and joiners:
            // what the font draws as a box. Common punctuation (U+2000-218F: dashes, bullet, ellipsis) stays.
            if ((code >= 0x2190 && code <= 0x2bff) || (code >= 0xe000 && code <= 0xf8ff) || (code >= 0xfe00 && code <= 0xfe0f) || code == 0x200d || code == 0x20e3 ||
                (code >= 0x3000 && code <= 0x303f && code != 0x3000))
                continue;
        }
        if (piece == " " && (out.empty() || out.back() == ' ')) continue;
        out += piece;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

}

bool FetchSwitchLobby(std::vector<LobbyRoom>& rooms, std::string& error) {
    rooms.clear();
    const HttpResult answer = HttpRequest("GET", kLobby, "", "", "RangaEmu", 15000);
    if (answer.status != 200) {
        error = answer.status == 0 ? "A lista de salas públicas não respondeu." : "A lista de salas públicas respondeu com erro " + std::to_string(answer.status) + ".";
        return false;
    }
    Json root;
    if (!Parser(answer.body).Parse(root)) {
        error = "A lista de salas públicas veio num formato que o Ranga não entende.";
        return false;
    }
    const Json* list = root.kind == Json::Object ? root.Get("rooms") : &root;
    if (list == nullptr || list->kind != Json::Array) {
        error = "A lista de salas públicas veio vazia.";
        return false;
    }
    for (const Json& item : list->items) {
        if (item.kind != Json::Object) continue;
        LobbyRoom room;
        room.name = Readable(item.Str("name"));
        room.description = Readable(item.Str("description"));
        room.address = item.Str("address");
        room.port = static_cast<int>(item.Int("port"));
        room.maxPlayers = static_cast<int>(item.Int("maxPlayers"));
        room.netVersion = static_cast<int>(item.Int("netVersion"));
        room.hasPassword = item.Flag("hasPassword");
        room.preferredGameName = Readable(item.Str("preferredGameName"));
        room.preferredGameId = item.Id("preferredGameId");
        if (const Json* players = item.Get("players"); players != nullptr && players->kind == Json::Array) {
            for (const Json& player : players->items) {
                if (player.kind != Json::Object) continue;
                room.players.push_back({Readable(player.Str("nickname")), Readable(player.Str("gameName")), player.Id("gameId")});
            }
        }
        if (room.address.empty() || room.port <= 0 || room.port > 65535 || room.netVersion != kNetworkVersion) continue;
        rooms.push_back(std::move(room));
    }
    return true;
}

int PingAddress(const std::string& address, int timeoutMs) {
    IN_ADDR target{};
    if (inet_pton(AF_INET, address.c_str(), &target) != 1) return -1;
    HANDLE icmp = IcmpCreateFile();
    if (icmp == INVALID_HANDLE_VALUE) return -1;
    char payload[8] = "ranga";
    const DWORD replySize = sizeof(ICMP_ECHO_REPLY) + sizeof(payload) + 8;
    auto reply = std::make_unique<char[]>(replySize);
    int result = -1;
    if (IcmpSendEcho(icmp, target.S_un.S_addr, payload, sizeof(payload), nullptr, reply.get(), replySize, static_cast<DWORD>(timeoutMs)) > 0) {
        const auto* echo = reinterpret_cast<const ICMP_ECHO_REPLY*>(reply.get());
        if (echo->Status == IP_SUCCESS) result = static_cast<int>(echo->RoundTripTime);
    }
    IcmpCloseHandle(icmp);
    return result;
}

uint64_t SwitchTitleId(const std::wstring& path) {
    for (size_t at = path.find(L'['); at != std::wstring::npos; at = path.find(L'[', at + 1)) {
        if (at + 17 >= path.size() || path[at + 17] != L']') continue;
        const std::wstring digits = path.substr(at + 1, 16);
        bool hex = true;
        for (const wchar_t c : digits) hex = hex && std::iswxdigit(c);
        if (hex && (digits[0] == L'0') && (digits[1] == L'1')) return std::wcstoull(digits.c_str(), nullptr, 16);
    }
    return 0;
}

std::string LooseTitle(const std::string& title) {
    std::string out;
    bool space = false;
    for (const unsigned char c : title) {
        if (std::isalnum(c)) {
            if (space && !out.empty()) out += ' ';
            out += static_cast<char>(std::tolower(c));
            space = false;
        } else {
            space = true;
        }
    }
    return out;
}

}
