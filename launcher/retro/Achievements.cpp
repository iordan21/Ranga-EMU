#include "Achievements.hpp"
#include "../Http.hpp"

#include "libretro.h"
#include "rc_client.h"
#include "rc_libretro.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cwctype>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace Achievements {

namespace {

struct Request {
    std::string url, post, type;
    rc_client_server_callback_t callback = nullptr;
    void* data = nullptr;
};

struct Reply {
    Ranga::HttpResult result;
    rc_client_server_callback_t callback = nullptr;
    void* data = nullptr;
};

struct Notice {
    std::wstring heading, title, detail;
    bool trophy = true;
};

struct State {
    Setup setup;
    uint32_t console = 0;
    rc_client_t* client = nullptr;
    std::string userAgent;
    rc_libretro_memory_regions_t regions{};
    std::vector<retro_memory_descriptor> descriptors;
    retro_memory_map map{};
    bool hasMap = false;
    ULONGLONG memoryTriedAt = 0;
    // The server calls go one at a time on a thread of their own; their answers reach rcheevos on the game's thread.
    std::mutex lock;
    std::condition_variable wake;
    std::deque<Request> requests;
    std::deque<Reply> replies;
    std::thread worker;
    bool stopping = false, busy = false;
    // The notices over the game, one at a time.
    std::deque<Notice> notices;
    Notice current;
    HWND popup = nullptr;
    ULONGLONG shownUntil = 0;
};

State* s = nullptr;
// The memory map a core may set before Start (while loading the game).
std::vector<retro_memory_descriptor> g_earlyDescriptors;
bool g_earlyMap = false;

std::wstring Wide(const char* text) {
    if (text == nullptr || *text == '\0') return L"";
    const int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(std::max(size, 1)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), size);
    out.resize(wcslen(out.c_str()));
    return out;
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return "";
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring LowerExtension(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || path.find_first_of(L"\\/", dot) != std::wstring::npos) return L"";
    std::wstring ext = path.substr(dot);
    for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));
    return ext;
}

// Ranga's console to RetroAchievements' (rc_consoles.h); 0 for the consoles it has no achievements for.
uint32_t ConsoleFor(const std::wstring& system, const std::wstring& rom) {
    const std::wstring ext = LowerExtension(rom);
    if (system == L"NES") return RC_CONSOLE_NINTENDO;
    if (system == L"SNES") return RC_CONSOLE_SUPER_NINTENDO;
    if (system == L"MD") return RC_CONSOLE_MEGA_DRIVE;
    if (system == L"SMS") return ext == L".gg" ? RC_CONSOLE_GAME_GEAR : RC_CONSOLE_MASTER_SYSTEM;
    if (system == L"SEGACD") return RC_CONSOLE_SEGA_CD;
    if (system == L"PCE") return RC_CONSOLE_PC_ENGINE;
    if (system == L"A2600") return RC_CONSOLE_ATARI_2600;
    if (system == L"ARCADE") return RC_CONSOLE_ARCADE;
    if (system == L"GB") return ext == L".gbc" ? RC_CONSOLE_GAMEBOY_COLOR : RC_CONSOLE_GAMEBOY;
    if (system == L"GBA") return RC_CONSOLE_GAMEBOY_ADVANCE;
    if (system == L"PS1") return RC_CONSOLE_PLAYSTATION;
    if (system == L"N64") return RC_CONSOLE_NINTENDO_64;
    if (system == L"SATURN") return RC_CONSOLE_SATURN;
    if (system == L"NDS") return RC_CONSOLE_NINTENDO_DS;
    if (system == L"DC") return RC_CONSOLE_DREAMCAST;
    if (system == L"GC") return RC_CONSOLE_GAMECUBE;
    if (system == L"PS2") return RC_CONSOLE_PLAYSTATION_2;
    if (system == L"PSP") return RC_CONSOLE_PSP;
    return 0;
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

// The value of a "key<TAB>value" line of perfil\perfil.txt.
std::string ProfileValue(const std::string& text, const std::string& key) {
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > key.size() && line.compare(0, key.size(), key) == 0 && line[key.size()] == '\t') return line.substr(key.size() + 1);
        at = end + 1;
    }
    return "";
}

void Log(const char* format, ...) {
    if (s == nullptr || !s->setup.log) return;
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    s->setup.log(text);
}

void Notify(const std::wstring& heading, const std::wstring& title, const std::wstring& detail, bool trophy = true) {
    if (s != nullptr) s->notices.push_back({heading, title, detail, trophy});
}

std::string Clean(const char* text) {
    std::string out = text != nullptr ? text : "";
    for (char& c : out) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return out;
}

// The game's achievements for Ranga's profile: workspace\retroachievements.txt, written whole each time.
void WriteList() {
    const rc_client_game_t* game = rc_client_get_game_info(s->client);
    if (game == nullptr || game->id == 0 || s->setup.workspace.empty()) return;
    std::string text = "ranga-ra 1\njogo\t" + std::to_string(game->id) + "\t" + Clean(game->badge_name) + "\t" + Clean(game->title) + "\n";
    rc_client_achievement_list_t* list = rc_client_create_achievement_list(s->client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    if (list != nullptr) {
        const long long now = static_cast<long long>(std::time(nullptr));
        for (uint32_t b = 0; b < list->num_buckets; ++b) {
            const rc_client_achievement_bucket_t& bucket = list->buckets[b];
            for (uint32_t i = 0; i < bucket.num_achievements; ++i) {
                const rc_client_achievement_t* a = bucket.achievements[i];
                const long long when = a->unlocked == 0 ? 0 : a->unlock_time != 0 ? static_cast<long long>(a->unlock_time) : now;
                text += "c\t" + std::to_string(a->id) + "\t" + std::to_string(a->points) + "\t" + std::to_string(when) + "\t" + Clean(a->badge_name) + "\t" +
                        Clean(a->title) + "\t" + Clean(a->description) + "\n";
            }
        }
        rc_client_destroy_achievement_list(list);
    }
    const std::wstring path = s->setup.workspace + L"\\retroachievements.txt", temporary = path + L".novo";
    FILE* file = nullptr;
    if (_wfopen_s(&file, temporary.c_str(), L"wb") != 0 || file == nullptr) return;
    const bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    std::fclose(file);
    if (ok) MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void RC_CCONV CoreMemoryInfo(uint32_t id, rc_libretro_core_memory_info_t* info) {
    info->data = static_cast<uint8_t*>(s->setup.memoryData ? s->setup.memoryData(id) : nullptr);
    info->size = s->setup.memorySize ? s->setup.memorySize(id) : 0;
    if (info->data == nullptr) info->size = 0;
}

void InitMemory() {
    const ULONGLONG now = GetTickCount64();
    if (s->regions.count != 0 || now - s->memoryTriedAt < 1000) return;
    s->memoryTriedAt = now;
    rc_libretro_memory_destroy(&s->regions);
    if (rc_libretro_memory_init(&s->regions, s->hasMap ? &s->map : nullptr, CoreMemoryInfo, s->console) && s->regions.count != 0)
        Log("[conquistas] memória do jogo: %u regiões, %zu bytes\n", s->regions.count, s->regions.total_size);
}

uint32_t RC_CCONV ReadMemory(uint32_t address, uint8_t* buffer, uint32_t bytes, rc_client_t*) {
    if (s == nullptr || s->regions.count == 0) return 0;
    return rc_libretro_memory_read(&s->regions, address, buffer, bytes);
}

void RC_CCONV ServerCall(const rc_api_request_t* request, rc_client_server_callback_t callback, void* data, rc_client_t*) {
    Request item;
    item.url = request->url != nullptr ? request->url : "";
    item.post = request->post_data != nullptr ? request->post_data : "";
    item.type = request->content_type != nullptr ? request->content_type : "application/x-www-form-urlencoded";
    item.callback = callback;
    item.data = data;
    {
        std::lock_guard guard(s->lock);
        s->requests.push_back(std::move(item));
    }
    s->wake.notify_one();
}

void Worker(State* state) {
    for (;;) {
        Request request;
        {
            std::unique_lock guard(state->lock);
            state->wake.wait(guard, [&] { return state->stopping || !state->requests.empty(); });
            if (state->stopping && state->requests.empty()) return;
            request = std::move(state->requests.front());
            state->requests.pop_front();
            state->busy = true;
        }
        Reply reply;
        reply.result = Ranga::HttpRequest(request.post.empty() ? "GET" : "POST", request.url, request.post, request.post.empty() ? "" : request.type, state->userAgent, 30000);
        reply.callback = request.callback;
        reply.data = request.data;
        std::lock_guard guard(state->lock);
        state->replies.push_back(std::move(reply));
        state->busy = false;
    }
}

void RC_CCONV ClientLog(const char* message, const rc_client_t*) { Log("[conquistas] %s\n", message); }

void RC_CCONV Event(const rc_client_event_t* event, rc_client_t*) {
    switch (event->type) {
        case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED: {
            const rc_client_achievement_t* a = event->achievement;
            Notify(L"Conquista desbloqueada  ·  " + std::to_wstring(a->points) + L" pontos", Wide(a->title), Wide(a->description));
            Log("[conquistas] desbloqueada: %s (%u)\n", a->title, a->id);
            WriteList();
            break;
        }
        case RC_CLIENT_EVENT_GAME_COMPLETED:
            Notify(L"RetroAchievements", L"Jogo dominado!", L"Você tem todas as conquistas deste jogo.");
            break;
        case RC_CLIENT_EVENT_SERVER_ERROR:
            if (event->server_error != nullptr) Log("[conquistas] erro do servidor em %s: %s\n", event->server_error->api, event->server_error->error_message);
            break;
        case RC_CLIENT_EVENT_DISCONNECTED:
            Notify(L"RetroAchievements", L"Sem conexão", L"As conquistas ficam guardadas e vão quando a internet voltar.", false);
            break;
        case RC_CLIENT_EVENT_RECONNECTED:
            Notify(L"RetroAchievements", L"Conectado de novo", L"As conquistas pendentes foram enviadas.", false);
            break;
        default:
            break;
    }
}

void RC_CCONV GameLoaded(int result, const char* error, rc_client_t* client, void*) {
    if (result == RC_NO_GAME_LOADED) {
        Notify(L"RetroAchievements", L"Jogo sem conquistas", L"O RetroAchievements não reconhece este arquivo (ou é outra versão do jogo).", false);
        Log("[conquistas] jogo não reconhecido\n");
        return;
    }
    if (result != RC_OK) {
        Notify(L"RetroAchievements", L"Não carregou as conquistas", Wide(error != nullptr ? error : "erro desconhecido"), false);
        Log("[conquistas] falhou ao carregar o jogo: %s\n", error != nullptr ? error : "?");
        return;
    }
    rc_client_user_game_summary_t summary{};
    rc_client_get_user_game_summary(client, &summary);
    const rc_client_game_t* game = rc_client_get_game_info(client);
    if (summary.num_core_achievements == 0) {
        Notify(L"RetroAchievements", Wide(game != nullptr ? game->title : ""), L"Este jogo ainda não tem conquistas.", false);
    } else {
        Notify(L"RetroAchievements  ·  modo normal", Wide(game != nullptr ? game->title : ""),
               std::to_wstring(summary.num_unlocked_achievements) + L" de " + std::to_wstring(summary.num_core_achievements) + L" conquistas  ·  " +
                   std::to_wstring(summary.points_unlocked) + L" de " + std::to_wstring(summary.points_core) + L" pontos",
               false);
    }
    Log("[conquistas] %s: %u de %u conquistas\n", game != nullptr ? game->title : "?", summary.num_unlocked_achievements, summary.num_core_achievements);
    WriteList();
}

void RC_CCONV LoggedIn(int result, const char* error, rc_client_t* client, void*) {
    if (result != RC_OK) {
        Notify(L"RetroAchievements", L"Não entrou na conta", L"Conecte a conta de novo em Editar perfil, no Ranga.", false);
        Log("[conquistas] login falhou: %s\n", error != nullptr ? error : "?");
        return;
    }
    const std::string rom = Utf8(s->setup.rom);
    rc_client_begin_identify_and_load_game(client, s->console, rom.c_str(), s->setup.data, s->setup.size, GameLoaded, nullptr);
}

// ---- The notice over the game: a small window of its own, owned by the game's, that clicks pass through ----

void PaintPopup(HWND window) {
    PAINTSTRUCT paint;
    HDC dc = BeginPaint(window, &paint);
    RECT client;
    GetClientRect(window, &client);
    const int height = client.bottom, pad = height * 14 / 92, medal = height - 2 * pad;
    HBRUSH background = CreateSolidBrush(RGB(0x17, 0x18, 0x1c));
    FillRect(dc, &client, background);
    DeleteObject(background);
    HBRUSH edge = CreateSolidBrush(s->current.trophy ? RGB(0xf5, 0xa5, 0x24) : RGB(0x3e, 0x8e, 0xd0));
    HGDIOBJ oldBrush = SelectObject(dc, edge);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, pad, pad, pad + medal, pad + medal, medal / 3, medal / 3);
    // A star in the square.
    POINT star[10];
    const double cx = pad + medal / 2.0, cy = pad + medal * 0.53;
    for (int i = 0; i < 10; ++i) {
        const double angle = -1.5707963 + i * 0.6283185, radius = medal * (i % 2 == 0 ? 0.33 : 0.14);
        star[i] = {static_cast<LONG>(cx + std::cos(angle) * radius), static_cast<LONG>(cy + std::sin(angle) * radius)};
    }
    SelectObject(dc, GetStockObject(WHITE_BRUSH));
    Polygon(dc, star, 10);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(edge);
    SetBkMode(dc, TRANSPARENT);
    const auto font = [&](int size, int weight) {
        return CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    HFONT small = font(height * 13 / 92, FW_NORMAL), big = font(height * 18 / 92, FW_SEMIBOLD);
    const int left = pad * 2 + medal, right = client.right - pad;
    const UINT flags = DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
    RECT line{left, pad - 2, right, pad + height * 18 / 92};
    HGDIOBJ oldFont = SelectObject(dc, small);
    SetTextColor(dc, RGB(0x8b, 0x8d, 0x93));
    DrawTextW(dc, s->current.heading.c_str(), -1, &line, flags);
    line = {left, pad + height * 20 / 92, right, pad + height * 44 / 92};
    SelectObject(dc, big);
    SetTextColor(dc, RGB(0xe8, 0xe8, 0xea));
    DrawTextW(dc, s->current.title.c_str(), -1, &line, flags);
    line = {left, pad + height * 46 / 92, right, client.bottom - pad / 2};
    SelectObject(dc, small);
    SetTextColor(dc, RGB(0x8b, 0x8d, 0x93));
    DrawTextW(dc, s->current.detail.c_str(), -1, &line, flags);
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

void ShowNotices() {
    if (s->setup.window == nullptr) return;
    const ULONGLONG now = GetTickCount64();
    if (s->shownUntil != 0 && now < s->shownUntil) return;
    if (s->notices.empty()) {
        if (s->shownUntil != 0 && s->popup != nullptr) ShowWindow(s->popup, SW_HIDE);
        s->shownUntil = 0;
        return;
    }
    if (s->popup == nullptr) {
        WNDCLASSW type{};
        type.lpfnWndProc = PopupProc;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"RangaConquista";
        type.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&type);
        s->popup = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"RangaConquista", L"", WS_POPUP, 0, 0, 10, 10,
                                   s->setup.window, nullptr, type.hInstance, nullptr);
        if (s->popup == nullptr) return;
        SetLayeredWindowAttributes(s->popup, 0, 242, LWA_ALPHA);
    }
    s->current = s->notices.front();
    s->notices.pop_front();
    s->shownUntil = now + (s->current.trophy ? 6000 : 4500);
    // Bottom left of the game, sized for the screen's DPI.
    RECT area;
    GetClientRect(s->setup.window, &area);
    POINT corner{0, area.bottom};
    ClientToScreen(s->setup.window, &corner);
    HDC screen = GetDC(s->setup.window);
    const int dpi = screen != nullptr ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
    if (screen != nullptr) ReleaseDC(s->setup.window, screen);
    const int width = 460 * dpi / 96, height = 92 * dpi / 96, margin = 24 * dpi / 96;
    SetWindowPos(s->popup, HWND_TOP, corner.x + margin, corner.y - height - margin, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(s->popup, nullptr, TRUE);
}

}

void SetMemoryMap(const retro_memory_map* map) {
    std::vector<retro_memory_descriptor> copy;
    if (map != nullptr && map->descriptors != nullptr) copy.assign(map->descriptors, map->descriptors + map->num_descriptors);
    if (s == nullptr) {
        g_earlyDescriptors = std::move(copy);
        g_earlyMap = map != nullptr;
        return;
    }
    s->descriptors = std::move(copy);
    s->hasMap = map != nullptr;
    s->map.descriptors = s->descriptors.data();
    s->map.num_descriptors = static_cast<unsigned>(s->descriptors.size());
    s->regions.count = 0;
    s->memoryTriedAt = 0;
}

void Start(const Setup& setup) {
    const uint32_t console = ConsoleFor(setup.system, setup.rom);
    if (console == 0 || s != nullptr) return;
    const std::string profile = ReadWhole(setup.programFolder + L"\\perfil\\perfil.txt");
    const std::string user = ProfileValue(profile, "ra_usuario"), token = ProfileValue(profile, "ra_token");
    if (user.empty() || token.empty()) return;
    s = new State;
    s->setup = setup;
    s->console = console;
    s->descriptors = std::move(g_earlyDescriptors);
    s->hasMap = g_earlyMap;
    s->map.descriptors = s->descriptors.data();
    s->map.num_descriptors = static_cast<unsigned>(s->descriptors.size());
    s->client = rc_client_create(ReadMemory, ServerCall);
    char clause[64] = {};
    rc_client_get_user_agent_clause(s->client, clause, sizeof(clause));
    s->userAgent = std::string("RangaEmu/2026.11 ") + clause;
    rc_client_enable_logging(s->client, RC_CLIENT_LOG_LEVEL_WARN, ClientLog);
    rc_client_set_event_handler(s->client, Event);
    rc_client_set_hardcore_enabled(s->client, 0);
    s->worker = std::thread(Worker, s);
    Log("[conquistas] entrando como %s (console %u)\n", user.c_str(), console);
    rc_client_begin_login_with_token(s->client, user.c_str(), token.c_str(), LoggedIn, nullptr);
}

bool Active() { return s != nullptr; }

void Idle() {
    if (s == nullptr) return;
    std::deque<Reply> replies;
    {
        std::lock_guard guard(s->lock);
        replies.swap(s->replies);
    }
    for (const Reply& reply : replies) {
        rc_api_server_response_t response{};
        response.body = reply.result.body.c_str();
        response.body_length = reply.result.body.size();
        response.http_status_code = reply.result.status != 0 ? reply.result.status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
        reply.callback(&response, reply.data);
    }
    if (rc_client_is_game_loaded(s->client)) InitMemory();
    ShowNotices();
}

void Frame() {
    if (s == nullptr) return;
    if (rc_client_is_game_loaded(s->client)) InitMemory();
    rc_client_do_frame(s->client);
}

void Reset() {
    if (s != nullptr) rc_client_reset(s->client);
}

void Stop() {
    if (s == nullptr) return;
    // An unlock still on its way to the server gets a few seconds to arrive before the game closes.
    for (int waited = 0; waited < 120; ++waited) {
        Idle();
        bool idle;
        {
            std::lock_guard guard(s->lock);
            idle = s->requests.empty() && s->replies.empty() && !s->busy;
        }
        if (idle) break;
        Sleep(50);
    }
    bool stuck;
    {
        std::lock_guard guard(s->lock);
        s->stopping = true;
        stuck = s->busy;
    }
    s->wake.notify_one();
    if (stuck) {
        // A request that hangs keeps its thread; the process ends right after anyway.
        s->worker.detach();
        return;
    }
    s->worker.join();
    rc_client_destroy(s->client);
    rc_libretro_memory_destroy(&s->regions);
    if (s->popup != nullptr) DestroyWindow(s->popup);
    delete s;
    s = nullptr;
}

}
