#include "Netplay.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "../net/RangaNet.hpp"

namespace Netplay {

namespace {

enum Message : uint8_t {
    kInput = RangaNet::kRelay, // u32 frame, input
    kState,                    // u32 frame, u8 present, 4 x u32 start, u16 n, n x (u32 frame, u8 slot, input), u32 size, packed state
    kCheck,                    // u32 frame, u32 crc
    kAgain,                    // (a player's state differs: slot 0 sends it again)
    kInputs,                   // u16 n, n x (u32 frame, input): a player's own controls again, for a newcomer
};

constexpr int kPlayers = RangaNet::kMaxPlayers;
constexpr uint32_t kCheckEvery = 300;
// Controls kept behind the frame running: a newcomer is handed these, and a state sent again can go back this far.
constexpr uint32_t kKeep = 600;

struct Frame {
    std::array<Input, kPlayers> input{};
    uint8_t have = 0;
};

struct Session {
    SOCKET socket = INVALID_SOCKET;
    Hooks hooks;
    std::string game, name;
    int slot = -1, delay = 4;
    std::vector<uint8_t> in, out;
    size_t outAt = 0;
    // The frame about to run, counted from when the table's game started.
    uint32_t frame = 0;
    // Whether this player has the table's state (slot 0 always has).
    bool synced = false;
    bool present[kPlayers] = {};
    uint32_t start[kPlayers] = {};
    std::string names[kPlayers];
    std::map<uint32_t, Frame> frames;
    // The next frame this player's own controls go out for.
    uint32_t nextSend = 0;
    std::map<uint32_t, uint32_t> hostChecks, ownChecks;
    ULONGLONG lastAgain = 0, waitingSince = 0, lastSync = 0;
    Input current;
};

Session* s = nullptr;
const Input kNone{};

void Log(const char* text) {
    if (s != nullptr && s->hooks.log != nullptr) s->hooks.log("[online] %s\n", text);
}

void Say(const std::wstring& text) {
    if (s != nullptr && s->hooks.message != nullptr) s->hooks.message(text);
}

std::wstring Wide(const std::string& text) {
    if (text.empty()) return L"";
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

uint32_t Crc32(const uint8_t* data, size_t size) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// A save state is mostly zeros (a console's memory early in a game): runs of them go as a count.
// Pieces: u32 zeros, u32 literal length, the literal bytes.
std::vector<uint8_t> Pack(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> out;
    out.reserve(raw.size() / 2);
    size_t at = 0;
    const auto put = [&](uint32_t v) { out.insert(out.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); };
    while (at < raw.size()) {
        size_t zeros = 0;
        while (at + zeros < raw.size() && raw[at + zeros] == 0) ++zeros;
        at += zeros;
        // The literal ends where 16 zeros in a row start.
        size_t literal = 0, run = 0;
        while (at + literal < raw.size()) {
            run = raw[at + literal] == 0 ? run + 1 : 0;
            ++literal;
            if (run == 16) { literal -= 16; break; }
        }
        put(static_cast<uint32_t>(zeros));
        put(static_cast<uint32_t>(literal));
        out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + literal));
        at += literal;
    }
    return out;
}

bool Unpack(const uint8_t* data, size_t size, size_t rawSize, std::vector<uint8_t>& raw) {
    raw.assign(rawSize, 0);
    size_t at = 0, to = 0;
    while (at + 8 <= size) {
        uint32_t zeros = 0, literal = 0;
        std::memcpy(&zeros, data + at, 4);
        std::memcpy(&literal, data + at + 4, 4);
        at += 8;
        if (to + zeros + literal > rawSize || at + literal > size) return false;
        to += zeros;
        std::memcpy(raw.data() + to, data + at, literal);
        to += literal;
        at += literal;
    }
    return at == size;
}

void Queue(const std::vector<uint8_t>& bytes) { s->out.insert(s->out.end(), bytes.begin(), bytes.end()); }

void PutInput(RangaNet::Writer& w, const Input& input) {
    w.U16(input.mask);
    for (const int16_t axis : input.axis) w.I16(axis);
}

Input GetInput(RangaNet::Reader& r) {
    Input input;
    input.mask = r.U16();
    for (int16_t& axis : input.axis) axis = r.I16();
    return input;
}

void Store(uint32_t frame, int slot, const Input& input) {
    if (slot < 0 || slot >= kPlayers) return;
    Frame& f = s->frames[frame];
    f.input[static_cast<size_t>(slot)] = input;
    f.have |= static_cast<uint8_t>(1u << slot);
}

bool Needs(uint32_t frame, int slot) { return s->present[slot] && frame >= s->start[slot] + static_cast<uint32_t>(s->delay); }

// This player's own controls from a little behind the frame running, for someone who just sat down.
void SendOwnAgain() {
    RangaNet::Writer w(kInputs);
    std::vector<std::pair<uint32_t, Input>> own;
    const uint32_t from = s->frame > kKeep ? s->frame - kKeep : 0;
    for (auto it = s->frames.lower_bound(from); it != s->frames.end(); ++it) {
        if (it->second.have & (1u << s->slot)) own.push_back({it->first, it->second.input[static_cast<size_t>(s->slot)]});
    }
    w.U16(static_cast<uint16_t>(std::min<size_t>(own.size(), 65535)));
    for (size_t i = 0; i < own.size() && i < 65535; ++i) {
        w.U32(own[i].first);
        PutInput(w, own[i].second);
    }
    Queue(w.Done());
}

// Slot 0's state as it is before the frame about to run, with every control it holds from there on.
void SendState() {
    const size_t size = s->hooks.stateSize();
    std::vector<uint8_t> raw(size);
    if (size == 0 || !s->hooks.saveState(raw.data(), size)) {
        Log("este core não salva estado: o online não tem como começar igual para todos");
        return;
    }
    const auto packed = Pack(raw);
    RangaNet::Writer w(kState);
    w.U32(s->frame);
    uint8_t present = 0;
    for (int i = 0; i < kPlayers; ++i) present |= static_cast<uint8_t>(s->present[i] << i);
    w.U8(present);
    for (const uint32_t start : s->start) w.U32(start);
    std::vector<std::tuple<uint32_t, int, Input>> held;
    for (auto it = s->frames.lower_bound(s->frame); it != s->frames.end(); ++it) {
        for (int i = 0; i < kPlayers; ++i) if (it->second.have & (1u << i)) held.emplace_back(it->first, i, it->second.input[static_cast<size_t>(i)]);
    }
    w.U16(static_cast<uint16_t>(std::min<size_t>(held.size(), 65535)));
    for (size_t i = 0; i < held.size() && i < 65535; ++i) {
        w.U32(std::get<0>(held[i]));
        w.U8(static_cast<uint8_t>(std::get<1>(held[i])));
        PutInput(w, std::get<2>(held[i]));
    }
    w.U32(static_cast<uint32_t>(raw.size()));
    w.Raw(packed.data(), packed.size());
    Queue(w.Done());
    s->lastSync = GetTickCount64();
    char text[160];
    std::snprintf(text, sizeof(text), "estado do quadro %u enviado: %zu bytes (%zu compactados)", s->frame, raw.size(), packed.size());
    Log(text);
}

void TakeState(RangaNet::Reader& r) {
    const uint32_t frame = r.U32();
    const uint8_t present = r.U8();
    uint32_t start[kPlayers];
    for (uint32_t& value : start) value = r.U32();
    const uint16_t held = r.U16();
    for (uint16_t i = 0; i < held && r.Ok(); ++i) {
        const uint32_t at = r.U32();
        const int slot = r.U8();
        const Input input = GetInput(r);
        if (slot != s->slot) Store(at, slot, input);
    }
    const uint32_t rawSize = r.U32();
    if (!r.Ok()) return;
    std::vector<uint8_t> raw;
    if (!Unpack(r.Here(), r.Left(), rawSize, raw) || !s->hooks.loadState(raw.data(), raw.size())) {
        Log("o estado que chegou não carregou");
        return;
    }
    s->frame = frame;
    for (int i = 0; i < kPlayers; ++i) {
        s->present[i] = (present >> i) & 1;
        s->start[i] = start[i];
    }
    // This player's controls start where the table says: before that, none.
    s->nextSend = std::max(s->nextSend, s->start[s->slot] + static_cast<uint32_t>(s->delay));
    if (!s->synced) Say(L"Online: entrou no jogo de " + Wide(s->names[0]));
    s->synced = true;
    s->ownChecks.clear();
    s->hostChecks.clear();
    char text[96];
    std::snprintf(text, sizeof(text), "estado do quadro %u carregado (%u bytes)", frame, rawSize);
    Log(text);
}

void Compare(uint32_t frame) {
    const auto host = s->hostChecks.find(frame), own = s->ownChecks.find(frame);
    if (host == s->hostChecks.end() || own == s->ownChecks.end()) return;
    if (host->second != own->second && GetTickCount64() - s->lastAgain > 3000) {
        char text[96];
        std::snprintf(text, sizeof(text), "o jogo saiu igual do anfitrião no quadro %u: pedindo o estado de novo", frame);
        Log(text);
        s->lastAgain = GetTickCount64();
        Queue(RangaNet::Writer(kAgain).Done());
    }
    s->hostChecks.erase(host);
    s->ownChecks.erase(own);
}

void Handle(uint8_t type, const uint8_t* body, size_t size) {
    RangaNet::Reader r(body, size);
    if (type == RangaNet::kWelcome) {
        s->slot = r.U8();
        const uint8_t count = r.U8();
        for (uint8_t i = 0; i < count && r.Ok(); ++i) {
            const int other = r.U8();
            const std::string name = r.Str();
            if (other < kPlayers) s->names[other] = name;
        }
        s->names[s->slot] = s->name;
        if (s->slot == 0) {
            // The table's game is this one: it starts here, at this frame.
            s->synced = true;
            s->present[0] = true;
            s->start[0] = s->frame;
            s->nextSend = s->frame + static_cast<uint32_t>(s->delay);
            Say(L"Online: você é o anfitrião; esperando os amigos");
        } else {
            Say(L"Online: entrando no jogo de " + Wide(s->names[0]) + L"...");
        }
        char text[96];
        std::snprintf(text, sizeof(text), "sentou no lugar %d (jogador %d)", s->slot, s->slot + 1);
        Log(text);
        return;
    }
    if (type == RangaNet::kJoin) {
        const int slot = r.U8();
        const std::string name = r.Str();
        if (slot >= kPlayers) return;
        s->names[slot] = name;
        Say(L"Online: " + Wide(name) + L" entrou (jogador " + std::to_wstring(slot + 1) + L")");
        if (s->synced) SendOwnAgain();
        if (s->slot == 0) {
            s->present[slot] = true;
            s->start[slot] = s->frame;
            SendState();
        }
        return;
    }
    if (type == RangaNet::kLeave) {
        const int slot = r.U8();
        if (slot >= kPlayers) return;
        s->present[slot] = false;
        Say(L"Online: " + Wide(s->names[slot]) + L" saiu");
        return;
    }
    if (type == RangaNet::kFull) {
        Say(L"Online: a mesa deste jogo já tem 4 jogadores");
        return;
    }
    if (type < RangaNet::kRelay) return;
    const int from = r.U8();
    if (from >= kPlayers) return;
    switch (type) {
        case kInput: {
            const uint32_t frame = r.U32();
            const Input input = GetInput(r);
            if (r.Ok()) Store(frame, from, input);
            break;
        }
        case kInputs: {
            const uint16_t n = r.U16();
            for (uint16_t i = 0; i < n && r.Ok(); ++i) {
                const uint32_t frame = r.U32();
                const Input input = GetInput(r);
                if (r.Ok()) Store(frame, from, input);
            }
            break;
        }
        case kState:
            if (from == 0 && s->slot != 0) TakeState(r);
            break;
        case kCheck: {
            const uint32_t frame = r.U32(), crc = r.U32();
            if (from == 0 && s->slot != 0 && r.Ok()) {
                s->hostChecks[frame] = crc;
                Compare(frame);
            }
            break;
        }
        case kAgain:
            if (s->slot == 0 && GetTickCount64() - s->lastSync > 3000) SendState();
            break;
    }
}

void Drop(const std::wstring& why) {
    if (s == nullptr) return;
    Say(why);
    std::string text(why.begin(), why.end());
    Log("a conexão acabou; o jogo segue sozinho");
    closesocket(s->socket);
    delete s;
    s = nullptr;
}

}

bool Start(const std::string& address, const std::string& game, const std::string& name, int delay, const Hooks& hooks) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    std::string host = address, port = std::to_string(RangaNet::kDefaultPort);
    const size_t colon = address.rfind(':');
    if (colon != std::string::npos && address.find(':') == colon) {
        host = address.substr(0, colon);
        if (colon + 1 < address.size()) port = address.substr(colon + 1);
    }
    s = new Session;
    s->hooks = hooks;
    s->game = game;
    s->name = name.empty() ? "Jogador" : name;
    s->delay = std::clamp(delay, 1, 15);
    addrinfo hints{}, *found = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0 || found == nullptr) {
        Say(L"Online: o endereço " + Wide(address) + L" não foi encontrado; jogando sozinho");
        Log("endereço não encontrado");
        delete s;
        s = nullptr;
        return false;
    }
    SOCKET sock = INVALID_SOCKET;
    for (addrinfo* a = found; a != nullptr && sock == INVALID_SOCKET; a = a->ai_next) {
        sock = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (sock == INVALID_SOCKET) continue;
        // Five seconds to connect, then the game runs alone.
        u_long nonblocking = 1;
        ioctlsocket(sock, FIONBIO, &nonblocking);
        connect(sock, a->ai_addr, static_cast<int>(a->ai_addrlen));
        fd_set writes, errors;
        FD_ZERO(&writes);
        FD_ZERO(&errors);
        FD_SET(sock, &writes);
        FD_SET(sock, &errors);
        timeval wait{5, 0};
        if (select(0, nullptr, &writes, &errors, &wait) != 1 || !FD_ISSET(sock, &writes)) {
            closesocket(sock);
            sock = INVALID_SOCKET;
        }
    }
    freeaddrinfo(found);
    if (sock == INVALID_SOCKET) {
        Say(L"Online: a sala em " + Wide(address) + L" não respondeu; jogando sozinho");
        Log("a sala não respondeu");
        delete s;
        s = nullptr;
        return false;
    }
    BOOL noDelay = TRUE;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    s->socket = sock;
    Queue(RangaNet::Writer(RangaNet::kHello).U8(RangaNet::kVersion).Str(game).Str(s->name).Done());
    s->hooks.log("[online] conectado a %s, jogo %s, atraso de %d quadros\n", address.c_str(), game.c_str(), s->delay);
    return true;
}

bool Active() { return s != nullptr; }
bool Hosting() { return s != nullptr && s->slot == 0; }

void Pump() {
    if (s == nullptr) return;
    for (;;) {
        uint8_t buffer[64 * 1024];
        const int got = recv(s->socket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
        if (got == 0 || (got < 0 && WSAGetLastError() != WSAEWOULDBLOCK)) {
            Drop(s->slot > 0 ? L"Online: o anfitrião saiu; o jogo segue sozinho" : L"Online: a sala caiu; o jogo segue sozinho");
            return;
        }
        if (got < 0) break;
        s->in.insert(s->in.end(), buffer, buffer + got);
    }
    if (!RangaNet::TakeMessages(s->in, [](uint8_t type, const uint8_t* body, size_t size) { if (s != nullptr) Handle(type, body, size); })) {
        Drop(L"Online: a sala mandou algo que não é do Ranga");
        return;
    }
    if (s == nullptr) return;
    while (s->outAt < s->out.size()) {
        const int sent = send(s->socket, reinterpret_cast<const char*>(s->out.data() + s->outAt), static_cast<int>(std::min<size_t>(s->out.size() - s->outAt, 1u << 20)), 0);
        if (sent <= 0) {
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
                Drop(L"Online: a sala caiu; o jogo segue sozinho");
                return;
            }
            break;
        }
        s->outAt += static_cast<size_t>(sent);
    }
    if (s->outAt == s->out.size()) {
        s->out.clear();
        s->outAt = 0;
    }
}

void Send(const Input& local) {
    if (s == nullptr || s->slot < 0 || !s->synced) return;
    s->current = local;
    // Every frame up to the one delay ahead has this player's controls on their way.
    while (s->nextSend <= s->frame + static_cast<uint32_t>(s->delay)) {
        Store(s->nextSend, s->slot, local);
        RangaNet::Writer w(kInput);
        w.U32(s->nextSend);
        PutInput(w, local);
        Queue(w.Done());
        ++s->nextSend;
    }
}

bool Ready() {
    if (s == nullptr) return true;
    bool ready = s->synced;
    if (ready) {
        const auto found = s->frames.find(s->frame);
        for (int i = 0; i < kPlayers && ready; ++i) {
            if (Needs(s->frame, i)) ready = found != s->frames.end() && (found->second.have & (1u << i));
        }
    }
    if (ready) s->waitingSince = 0;
    else if (s->waitingSince == 0) s->waitingSince = GetTickCount64();
    return ready;
}

const Input& InputOf(unsigned port) {
    if (s == nullptr || port >= kPlayers) return kNone;
    const auto found = s->frames.find(s->frame);
    if (found == s->frames.end() || !(found->second.have & (1u << port))) return kNone;
    return found->second.input[port];
}

void FrameDone() {
    if (s == nullptr) return;
    // RANGA_ONLINE_DEBUG: how many frames each player had a button down, shown with the checks.
    static int pressed[kPlayers] = {};
    if (const auto ran = s->frames.find(s->frame); ran != s->frames.end()) {
        for (int i = 0; i < kPlayers; ++i) pressed[i] += (ran->second.have & (1u << i)) && ran->second.input[static_cast<size_t>(i)].mask != 0;
    }
    ++s->frame;
    if (s->frame > kKeep) s->frames.erase(s->frames.begin(), s->frames.lower_bound(s->frame - kKeep));
    // Before the next frame runs, its state is the one checked, everywhere the same frame.
    bool others = false;
    for (int i = 0; i < kPlayers; ++i) others = others || (i != s->slot && s->present[i]);
    if (!others || s->frame % kCheckEvery != 0) return;
    const size_t size = s->hooks.stateSize();
    std::vector<uint8_t> raw(size);
    if (size == 0 || !s->hooks.saveState(raw.data(), size)) return;
    const uint32_t crc = Crc32(raw.data(), raw.size());
    // RANGA_ONLINE_DEBUG=1: every check in the log, on every PC, for comparing them side by side.
    static const bool debug = std::getenv("RANGA_ONLINE_DEBUG") != nullptr;
    if (debug) s->hooks.log("[online] verificação do quadro %u: %08x (quadros com botão: j1 %d, j2 %d)\n", s->frame, crc, pressed[0], pressed[1]);
    if (s->slot == 0) {
        Queue(RangaNet::Writer(kCheck).U32(s->frame).U32(crc).Done());
    } else {
        s->ownChecks[s->frame] = crc;
        Compare(s->frame);
        while (s->ownChecks.size() > 8) s->ownChecks.erase(s->ownChecks.begin());
        while (s->hostChecks.size() > 8) s->hostChecks.erase(s->hostChecks.begin());
    }
}

void Resync() {
    if (s != nullptr && s->slot == 0) SendState();
}

std::wstring Status() {
    if (s == nullptr) return L"";
    if (s->slot < 0) return L"online: conectando";
    if (!s->synced) return L"online: recebendo o jogo do anfitrião";
    int players = 0;
    for (const bool here : s->present) players += here;
    if (s->waitingSince != 0 && GetTickCount64() - s->waitingSince > 400) return L"online: esperando o outro jogador";
    if (players <= 1) return L"online: esperando amigos";
    return L"online: " + std::to_wstring(players) + L" jogadores";
}

void Stop() {
    if (s == nullptr) return;
    // What was queued still goes (a last input, a goodbye) before the line closes.
    if (s->outAt < s->out.size()) send(s->socket, reinterpret_cast<const char*>(s->out.data() + s->outAt), static_cast<int>(s->out.size() - s->outAt), 0);
    shutdown(s->socket, SD_BOTH);
    closesocket(s->socket);
    delete s;
    s = nullptr;
}

}
