#include "Social.hpp"
#include "../Http.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace Social {

namespace {

constexpr const char* kDefaultCentral = "https://ranga-central.nadroi-dev.workers.dev";
// Messages every four seconds; the friends list (who is online, what they play) every fifth time.
constexpr int kPollMs = 4000;
constexpr int kFriendsEvery = 5;

struct Notice {
    std::wstring title, text;
};

struct State {
    std::string central, session, me;
    HWND window = nullptr;
    std::mutex lock;
    std::condition_variable wake;
    std::thread worker;
    bool stopping = false, ready = false, changed = false;
    std::vector<Friend> friends;
    std::vector<Message> messages; // all of this session's account, oldest first
    long long after = 0;           // the newest message number seen
    long long startedAt = 0;       // messages before the game opened are not "new"
    std::map<std::string, long long> seenUpTo; // per friend, the newest number seen in the chat page
    std::deque<std::pair<std::string, std::string>> outbox;
    std::deque<Notice> notices;
    // The notice window.
    HWND popup = nullptr;
    Notice current;
    ULONGLONG shownUntil = 0;
};

State* s = nullptr;

std::wstring Wide(const std::string& text) {
    if (text.empty()) return L"";
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return "";
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::string ReadWhole(const std::wstring& path) {
    std::string bytes;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) return bytes;
    char block[4096];
    for (size_t got; (got = std::fread(block, 1, sizeof(block), file)) > 0;) bytes.append(block, got);
    std::fclose(file);
    return bytes;
}

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        at = end + 1;
    }
    return lines;
}

std::vector<std::string> Split(const std::string& line) {
    std::vector<std::string> parts;
    size_t at = 0;
    for (;;) {
        const size_t tab = line.find('\t', at);
        parts.push_back(line.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
        if (tab == std::string::npos) break;
        at = tab + 1;
    }
    return parts;
}

std::string Value(const std::string& text, const std::string& key) {
    for (const std::string& line : Lines(text)) {
        if (line.size() > key.size() && line.compare(0, key.size(), key) == 0 && line[key.size()] == '\t') return line.substr(key.size() + 1);
    }
    return "";
}

Ranga::HttpResult Call(const std::string& method, const std::string& path, const std::string& body) {
    return Ranga::HttpRequest(method, s->central + path, body, method == "POST" ? "text/plain; charset=utf-8" : "", "RangaEmu", 10000,
                              "Authorization: Bearer " + s->session + "\r\n");
}

// GET /amigos: "amigo<TAB>id<TAB>Nome#1234<TAB>visto" lines, then each friend's card (between "ranga-perfil 1" and
// "fim_cartao"), whose "jogando<TAB>title<TAB>console" says what they play.
std::vector<Friend> ReadFriends(const std::string& body, std::string& me) {
    std::vector<Friend> list;
    std::map<std::string, size_t> at;
    std::string card;
    for (const std::string& line : Lines(body)) {
        const auto parts = Split(line);
        if (parts[0] == "eu" && parts.size() >= 2) me = parts[1];
        else if (parts[0] == "amigo" && parts.size() >= 4) {
            Friend f;
            f.id = parts[1];
            f.name = parts[2];
            f.seen = std::atoll(parts[3].c_str());
            at[f.id] = list.size();
            list.push_back(f);
        } else if (parts[0] == "id" && parts.size() >= 2) {
            card = parts[1];
        } else if (parts[0] == "jogando" && parts.size() >= 2 && at.count(card) != 0) {
            list[at[card]].playing = parts[1];
            if (parts.size() >= 3) list[at[card]].system = parts[2];
        } else if (parts[0] == "visto" && parts.size() >= 2 && at.count(card) != 0) {
            list[at[card]].seen = std::max(list[at[card]].seen, std::atoll(parts[1].c_str()));
        } else if (parts[0] == "fim_cartao") {
            card.clear();
        }
    }
    std::sort(list.begin(), list.end(), [](const Friend& a, const Friend& b) {
        if (a.Online() != b.Online()) return a.Online();
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return list;
}

std::string NameOf(const std::string& id) {
    for (const Friend& f : s->friends) {
        if (f.id == id) return f.name.substr(0, f.name.rfind('#'));
    }
    return "Amigo";
}

void Work() {
    bool first = true;
    int round = 0;
    for (;;) {
        std::deque<std::pair<std::string, std::string>> outbox;
        long long after;
        bool ready;
        {
            std::unique_lock guard(s->lock);
            if (!first) s->wake.wait_for(guard, std::chrono::milliseconds(kPollMs), [] { return s->stopping || !s->outbox.empty(); });
            if (s->stopping) return;
            outbox.swap(s->outbox);
            after = s->after;
            ready = s->ready;
        }
        first = false;
        for (const auto& [to, text] : outbox) {
            const Ranga::HttpResult sent = Call("POST", "/mensagem", "para\t" + to + "\ntexto\t" + text + "\n");
            if (sent.status != 200) {
                const std::string why = Value(sent.body, "erro");
                std::lock_guard guard(s->lock);
                s->notices.push_back({L"A mensagem não foi", Wide(why.empty() ? "Sem conexão com a central." : why)});
            }
        }
        std::string me;
        const Ranga::HttpResult friends = (round++ % kFriendsEvery == 0 || !ready) ? Call("GET", "/amigos", "") : Ranga::HttpResult{};
        const Ranga::HttpResult got = Call("GET", "/mensagens?desde=" + std::to_string(after), "");
        std::lock_guard guard(s->lock);
        if (friends.status == 200) {
            s->friends = ReadFriends(friends.body, me);
            if (!me.empty()) s->me = me;
            s->ready = true;
            s->changed = true;
        }
        if (got.status == 200) {
            for (const std::string& line : Lines(got.body)) {
                const auto parts = Split(line);
                if (parts[0] != "m" || parts.size() < 6) continue;
                Message m;
                m.number = std::atoll(parts[1].c_str());
                m.when = std::atoll(parts[2].c_str());
                m.from = parts[3];
                m.to = parts[4];
                m.text = parts[5];
                if (m.number <= s->after) continue;
                s->after = m.number;
                // The server's copy of one sent from here takes the place of the one shown while it went.
                if (m.from == s->me) {
                    const auto mine = std::find_if(s->messages.begin(), s->messages.end(),
                                                   [&](const Message& x) { return x.number == -1 && x.to == m.to && x.text == m.text; });
                    if (mine != s->messages.end()) s->messages.erase(mine);
                }
                s->messages.push_back(m);
                s->changed = true;
                if (m.from != s->me && m.when >= s->startedAt) s->notices.push_back({L"Mensagem de " + Wide(NameOf(m.from)), Shown(m.text)});
            }
            if (s->messages.size() > 1000) s->messages.erase(s->messages.begin(), s->messages.end() - 1000);
        }
    }
}

// ---- The notice over the game (bottom right, so it does not cover an achievement's) ----

void PaintPopup(HWND window) {
    PAINTSTRUCT paint;
    HDC dc = BeginPaint(window, &paint);
    RECT client;
    GetClientRect(window, &client);
    const int height = client.bottom, pad = height * 14 / 92, icon = height - 2 * pad;
    HBRUSH background = CreateSolidBrush(RGB(0x17, 0x18, 0x1c));
    FillRect(dc, &client, background);
    DeleteObject(background);
    // A speech bubble in the blue square.
    HBRUSH blue = CreateSolidBrush(RGB(0x3e, 0x8e, 0xd0));
    HGDIOBJ oldBrush = SelectObject(dc, blue);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, pad, pad, pad + icon, pad + icon, icon / 3, icon / 3);
    SelectObject(dc, GetStockObject(WHITE_BRUSH));
    RoundRect(dc, pad + icon * 22 / 100, pad + icon * 26 / 100, pad + icon * 78 / 100, pad + icon * 64 / 100, icon / 6, icon / 6);
    POINT tail[3] = {{pad + icon * 34 / 100, pad + icon * 60 / 100}, {pad + icon * 50 / 100, pad + icon * 60 / 100}, {pad + icon * 32 / 100, pad + icon * 76 / 100}};
    Polygon(dc, tail, 3);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(blue);
    SetBkMode(dc, TRANSPARENT);
    const auto font = [&](int size, int weight) {
        return CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    HFONT small = font(height * 13 / 92, FW_NORMAL), big = font(height * 17 / 92, FW_SEMIBOLD);
    const int left = pad * 2 + icon, right = client.right - pad;
    HGDIOBJ oldFont = SelectObject(dc, small);
    RECT line{left, pad - 2, right, pad + height * 18 / 92};
    SetTextColor(dc, RGB(0x8b, 0x8d, 0x93));
    DrawTextW(dc, s->current.title.c_str(), -1, &line, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    line = {left, pad + height * 20 / 92, right, client.bottom - pad / 2};
    SelectObject(dc, big);
    SetTextColor(dc, RGB(0xe8, 0xe8, 0xea));
    DrawTextW(dc, s->current.text.c_str(), -1, &line, DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX | DT_EDITCONTROL);
    SelectObject(dc, oldFont);
    DeleteObject(small);
    DeleteObject(big);
    EndPaint(window, &paint);
}

LRESULT CALLBACK PopupProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT:
            if (s != nullptr) PaintPopup(window);
            else ValidateRect(window, nullptr);
            return 0;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}

bool Friend::Online() const { return seen > 0 && static_cast<long long>(std::time(nullptr)) - seen < 300; }

void Start(const std::wstring& programFolder, HWND window) {
    if (s != nullptr) return;
    const std::string profile = ReadWhole(programFolder + L"\\perfil\\perfil.txt");
    const std::string session = Value(profile, "sessao");
    wchar_t central[512] = {};
    GetPrivateProfileStringW(L"Ranga", L"central", L"", central, 512, (programFolder + L"\\engines.ini").c_str());
    std::string address = Utf8(central);
    if (session.empty() || address == "nenhuma") return;
    while (!address.empty() && address.back() == '/') address.pop_back();
    s = new State;
    s->central = address.empty() ? kDefaultCentral : address;
    s->session = session;
    s->me = Value(profile, "conta_id");
    s->window = window;
    s->startedAt = static_cast<long long>(std::time(nullptr)) - 5;
    s->worker = std::thread(Work);
}

std::wstring Shown(const std::string& text) {
    if (text.rfind("[[convite|", 0) == 0 && text.size() > 12) {
        const size_t bar = text.rfind('|');
        return L"Convite para jogar " + Wide(text.substr(bar + 1, text.size() - bar - 3));
    }
    if (text.rfind("[[imagem|", 0) == 0) return L"Imagem (abra no Ranga para ver)";
    return Wide(text);
}

bool Active() { return s != nullptr; }

bool Ready() {
    if (s == nullptr) return false;
    std::lock_guard guard(s->lock);
    return s->ready;
}

std::string Me() {
    if (s == nullptr) return "";
    std::lock_guard guard(s->lock);
    return s->me;
}

std::vector<Friend> Friends() {
    if (s == nullptr) return {};
    std::lock_guard guard(s->lock);
    return s->friends;
}

std::vector<Message> Conversation(const std::string& friendId, size_t last) {
    std::vector<Message> out;
    if (s == nullptr) return out;
    std::lock_guard guard(s->lock);
    for (const Message& m : s->messages) {
        if (m.from == friendId || m.to == friendId) out.push_back(m);
    }
    if (out.size() > last) out.erase(out.begin(), out.end() - static_cast<long>(last));
    return out;
}

int Unread(const std::string& friendId) {
    if (s == nullptr) return 0;
    std::lock_guard guard(s->lock);
    const long long seen = s->seenUpTo[friendId];
    int count = 0;
    for (const Message& m : s->messages) {
        if (m.from == friendId && m.when >= s->startedAt && m.number > seen) ++count;
    }
    return count;
}

void MarkSeen(const std::string& friendId) {
    if (s == nullptr) return;
    std::lock_guard guard(s->lock);
    s->seenUpTo[friendId] = s->after;
}

void Send(const std::string& to, const std::string& text) {
    if (s == nullptr || text.empty()) return;
    std::string clean = text;
    for (char& c : clean) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    {
        std::lock_guard guard(s->lock);
        s->outbox.push_back({to, clean});
        // Shown at once in the chat; the server's copy (with its number) replaces it on the next poll.
        Message mine;
        mine.number = -1; // not on the server yet
        mine.when = static_cast<long long>(std::time(nullptr));
        mine.from = s->me;
        mine.to = to;
        mine.text = clean;
        s->messages.push_back(mine);
        s->changed = true;
    }
    s->wake.notify_one();
}

bool Changed() {
    if (s == nullptr) return false;
    std::lock_guard guard(s->lock);
    const bool was = s->changed;
    s->changed = false;
    return was;
}

void Idle() {
    if (s == nullptr || s->window == nullptr) return;
    const ULONGLONG now = GetTickCount64();
    if (s->shownUntil != 0 && now < s->shownUntil) return;
    Notice next;
    {
        std::lock_guard guard(s->lock);
        if (s->notices.empty()) {
            if (s->shownUntil != 0 && s->popup != nullptr) ShowWindow(s->popup, SW_HIDE);
            s->shownUntil = 0;
            return;
        }
        next = s->notices.front();
        s->notices.pop_front();
    }
    if (s->popup == nullptr) {
        WNDCLASSW type{};
        type.lpfnWndProc = PopupProc;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"RangaMensagem";
        type.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&type);
        s->popup = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"RangaMensagem", L"", WS_POPUP, 0, 0, 10, 10,
                                   s->window, nullptr, type.hInstance, nullptr);
        if (s->popup == nullptr) return;
        SetLayeredWindowAttributes(s->popup, 0, 242, LWA_ALPHA);
    }
    s->current = next;
    s->shownUntil = now + 6000;
    RECT area;
    GetClientRect(s->window, &area);
    POINT corner{area.right, area.bottom};
    ClientToScreen(s->window, &corner);
    HDC screen = GetDC(s->window);
    const int dpi = screen != nullptr ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
    if (screen != nullptr) ReleaseDC(s->window, screen);
    const int width = 420 * dpi / 96, height = 92 * dpi / 96, margin = 24 * dpi / 96;
    SetWindowPos(s->popup, HWND_TOP, corner.x - width - margin, corner.y - height - margin, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(s->popup, nullptr, TRUE);
}

void Stop() {
    if (s == nullptr) return;
    {
        std::lock_guard guard(s->lock);
        s->stopping = true;
    }
    s->wake.notify_one();
    // A message still going out gets its chance: the worker sends the outbox before it stops only on its next turn,
    // so it is sent here if it is still waiting.
    if (s->worker.joinable()) s->worker.join();
    for (const auto& [to, text] : s->outbox) Call("POST", "/mensagem", "para\t" + to + "\ntexto\t" + text + "\n");
    if (s->popup != nullptr) DestroyWindow(s->popup);
    delete s;
    s = nullptr;
}

}
