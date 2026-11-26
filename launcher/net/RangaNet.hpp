#ifndef LAUNCHER_NET_RANGANET_HPP
#define LAUNCHER_NET_RANGANET_HPP

// The wire of Ranga's online play for the consoles whose cores have none of their own (NES to PS1 and N64): one TCP
// stream per player to the table server (tools\ranga-sala.exe), which pairs everyone running the same game and passes
// each player's messages on to the others.
//
// Every message: u32 length (of what follows, little-endian), u8 type, then its fields, little-endian. Strings are a
// u16 length and UTF-8 bytes.
//
//   player -> server  kHello    u8 version, str game, str name
//   server -> player  kWelcome  u8 slot (0 runs the game for everyone), u8 count, count x (u8 slot, str name)
//   server -> player  kJoin     u8 slot, str name
//   server -> player  kLeave    u8 slot
//   server -> player  kFull     (the table has four players already)
//   server -> player  kDenied   (the server has a group code and the player's Hello did not bring it)
//
// Since 1.8.2 the Hello carries two more strings after the name: the game's title and the group's code (the hosting
// PC's engines.ini [Ranga] grupo). A server started with --grupo seats only who brings it, so a stranger who knows the
// address cannot sit first at a table and hand the friends a game state of their own.
//
// Types from kRelay up are the players' own: the server sends each to the other players of the table as it came, with
// the sender's slot put in after the type.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace RangaNet {

constexpr uint8_t kVersion = 1;
constexpr int kDefaultPort = 24873;
constexpr int kMaxPlayers = 4;
// More than this in one message is a broken stream, not a game's state.
constexpr uint32_t kMaxMessage = 128u << 20;

enum Type : uint8_t { kHello = 1, kWelcome, kJoin, kLeave, kFull, kDenied, kRelay = 16 };
// What a connection may send before it sits at a table (its Hello): a stranger cannot make the server hold more.
constexpr size_t kMaxBeforeSeat = 64u * 1024;

class Writer {
public:
    explicit Writer(uint8_t type) { bytes.resize(4); U8(type); }
    Writer& U8(uint8_t v) { bytes.push_back(v); return *this; }
    Writer& U16(uint16_t v) { return Raw(&v, 2); }
    Writer& U32(uint32_t v) { return Raw(&v, 4); }
    Writer& I16(int16_t v) { return Raw(&v, 2); }
    Writer& Str(const std::string& s) { U16(static_cast<uint16_t>(std::min<size_t>(s.size(), 65535))); return Raw(s.data(), std::min<size_t>(s.size(), 65535)); }
    Writer& Raw(const void* data, size_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        bytes.insert(bytes.end(), p, p + size);
        return *this;
    }
    // The finished message, its length filled in.
    std::vector<uint8_t>& Done() {
        const uint32_t length = static_cast<uint32_t>(bytes.size() - 4);
        std::memcpy(bytes.data(), &length, 4);
        return bytes;
    }

private:
    std::vector<uint8_t> bytes;
};

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : at(data), end(data + size) {}
    bool Ok() const { return ok; }
    size_t Left() const { return static_cast<size_t>(end - at); }
    const uint8_t* Here() const { return at; }
    uint8_t U8() { uint8_t v = 0; Raw(&v, 1); return v; }
    uint16_t U16() { uint16_t v = 0; Raw(&v, 2); return v; }
    uint32_t U32() { uint32_t v = 0; Raw(&v, 4); return v; }
    int16_t I16() { int16_t v = 0; Raw(&v, 2); return v; }
    std::string Str() {
        const uint16_t n = U16();
        if (!ok || Left() < n) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(at), n);
        at += n;
        return s;
    }
    void Raw(void* out, size_t n) {
        if (!ok || Left() < n) { ok = false; std::memset(out, 0, n); return; }
        std::memcpy(out, at, n);
        at += n;
    }

private:
    const uint8_t* at;
    const uint8_t* end;
    bool ok = true;
};

// Takes whole messages off the front of what a stream delivered: each as (type, body after the type).
template <typename Handle> bool TakeMessages(std::vector<uint8_t>& stream, Handle&& handle) {
    size_t at = 0;
    while (stream.size() - at >= 5) {
        uint32_t length = 0;
        std::memcpy(&length, stream.data() + at, 4);
        if (length == 0 || length > kMaxMessage) return false;
        if (stream.size() - at - 4 < length) break;
        handle(stream[at + 4], stream.data() + at + 5, static_cast<size_t>(length - 1));
        at += 4 + length;
    }
    stream.erase(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(at));
    return true;
}

}

#endif
