#include "Covers.hpp"

#include <windows.h>
#include <winhttp.h>

#include <cctype>
#include <cstring>
#include <cwctype>
#include <utility>

namespace Ranga {

namespace {

constexpr const wchar_t* kHost = L"thumbnails.libretro.com";

// "%20"-style escapes of the UTF-8 bytes, for one part of a URL path.
std::wstring Escape(const std::wstring& part) {
    static const char* hex = "0123456789ABCDEF";
    std::wstring out;
    for (const unsigned char c : Utf8(part)) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<wchar_t>(c);
        } else {
            out += L'%';
            out += static_cast<wchar_t>(hex[c >> 4]);
            out += static_cast<wchar_t>(hex[c & 15]);
        }
    }
    return out;
}

// The href of a directory listing back to its file name: %-escapes and the HTML entities Apache writes.
std::wstring Unescape(const std::string& href) {
    std::string bytes;
    for (size_t i = 0; i < href.size(); ++i) {
        if (href[i] == '%' && i + 2 < href.size() && std::isxdigit(static_cast<unsigned char>(href[i + 1])) && std::isxdigit(static_cast<unsigned char>(href[i + 2]))) {
            bytes += static_cast<char>(std::stoi(href.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else if (href[i] == '&') {
            static const std::pair<const char*, char> entities[] = {{"&amp;", '&'}, {"&quot;", '"'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&#39;", '\''}};
            bool found = false;
            for (const auto& [entity, c] : entities) {
                if (href.compare(i, std::strlen(entity), entity) == 0) {
                    bytes += c;
                    i += std::strlen(entity) - 1;
                    found = true;
                    break;
                }
            }
            if (!found) bytes += '&';
        } else {
            bytes += href[i];
        }
    }
    return Wide(bytes);
}

// GET https://thumbnails.libretro.com<path>; the HTTP status, or 0 when the server could not be reached.
int Get(const std::wstring& path, std::string& body) {
    body.clear();
    HINTERNET session = WinHttpOpen(L"RangaEmu", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) session = WinHttpOpen(L"RangaEmu", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) return 0;
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 15000);
    int status = 0;
    if (HINTERNET connection = WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0)) {
        if (HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) {
            if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)) {
                DWORD code = 0, size = sizeof(code);
                WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
                bool complete = true;
                for (;;) {
                    DWORD available = 0;
                    if (!WinHttpQueryDataAvailable(request, &available)) { complete = false; break; }
                    if (available == 0) break;
                    const size_t at = body.size();
                    body.resize(at + available);
                    DWORD read = 0;
                    if (!WinHttpReadData(request, body.data() + at, available, &read)) { complete = false; break; }
                    body.resize(at + read);
                }
                status = complete ? static_cast<int>(code) : 0;
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connection);
    }
    WinHttpCloseHandle(session);
    return status;
}

bool WriteAll(const std::wstring& path, const std::string& bytes) {
    const std::wstring temporary = path + L".part";
    FILE* file = nullptr;
    if (_wfopen_s(&file, temporary.c_str(), L"wb") != 0 || file == nullptr) return false;
    const bool written = fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    fclose(file);
    if (!written || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

wchar_t Plain(wchar_t c) {
    static const std::wstring accented = L"àáâãäåçèéêëìíîïñòóôõöùúûüýÿ";
    static const std::wstring plain = L"aaaaaaceeeeiiiinooooouuuuyy";
    const size_t at = accented.find(c);
    return at == std::wstring::npos ? c : plain[at];
}

// Every (tag) of a name, lower-case, as one string.
std::wstring Tags(const std::wstring& name) {
    std::wstring tags;
    int depth = 0;
    for (const wchar_t c : name) {
        if (c == L'(' || c == L'[') ++depth;
        if (depth > 0) tags += static_cast<wchar_t>(std::towlower(c));
        if ((c == L')' || c == L']') && depth > 0) { --depth; tags += L' '; }
    }
    return tags;
}

struct Entry {
    std::wstring key, name;
};

std::wstring Best(const std::vector<Entry>& entries, const std::wstring& name) {
    const std::wstring key = CoverKey(name);
    if (key.empty()) return L"";
    const std::wstring wanted = Tags(name);
    static const wchar_t* regions[] = {L"brazil", L"usa", L"world", L"europe", L"japan", L"korea", L"spain", L"france", L"germany", L"italy", L"australia"};
    static const wchar_t* unreleased[] = {L"beta", L"proto", L"demo", L"sample", L"kiosk", L"debug", L"promo"};
    std::wstring best;
    double bestScore = -1e9;
    for (const Entry& entry : entries) {
        if (entry.key != key) continue;
        const std::wstring tags = Tags(entry.name);
        double score = -static_cast<double>(entry.name.size()) * 0.01;
        for (const wchar_t* word : unreleased) if (tags.find(word) != std::wstring::npos) score -= 100;
        for (const wchar_t* region : regions) {
            if (tags.find(region) == std::wstring::npos) continue;
            if (wanted.find(region) != std::wstring::npos) score += 100;
        }
        if (tags.find(L"brazil") != std::wstring::npos) score += 30;
        else if (tags.find(L"usa") != std::wstring::npos) score += 25;
        else if (tags.find(L"world") != std::wstring::npos) score += 20;
        else if (tags.find(L"europe") != std::wstring::npos) score += 15;
        if (tags.find(L"rev ") != std::wstring::npos) score -= 2;
        if (score > bestScore) {
            bestScore = score;
            best = entry.name;
        }
    }
    return best;
}

}

std::wstring BoxartFile(const Game& game) { return game.workspace + L"\\art\\boxart.png"; }

std::wstring CoverFolder(const std::wstring& system, const std::wstring& extension) {
    if (system == L"GB") return extension == L".gbc" ? L"Nintendo - Game Boy Color" : L"Nintendo - Game Boy";
    if (system == L"SMS") return extension == L".gg" ? L"Sega - Game Gear" : L"Sega - Master System - Mark III";
    if (system == L"PCE") {
        if (extension == L".sgx") return L"NEC - PC Engine SuperGrafx";
        if (extension == L".cue" || extension == L".chd" || extension == L".ccd" || extension == L".iso" || extension == L".m3u") return L"NEC - PC Engine CD - TurboGrafx-CD";
        return L"NEC - PC Engine - TurboGrafx 16";
    }
    static const std::pair<const wchar_t*, const wchar_t*> folders[] = {
        {L"SNES", L"Nintendo - Super Nintendo Entertainment System"}, {L"NES", L"Nintendo - Nintendo Entertainment System"},
        {L"N64", L"Nintendo - Nintendo 64"}, {L"GBA", L"Nintendo - Game Boy Advance"}, {L"NDS", L"Nintendo - Nintendo DS"},
        {L"3DS", L"Nintendo - Nintendo 3DS"}, {L"GC", L"Nintendo - GameCube"}, {L"WII", L"Nintendo - Wii"}, {L"WIIU", L"Nintendo - Wii U"},
        {L"MD", L"Sega - Mega Drive - Genesis"}, {L"SEGACD", L"Sega - Mega-CD - Sega CD"}, {L"SATURN", L"Sega - Saturn"},
        {L"DC", L"Sega - Dreamcast"}, {L"PS1", L"Sony - PlayStation"}, {L"PS2", L"Sony - PlayStation 2"},
        {L"PS3", L"Sony - PlayStation 3"}, {L"PSP", L"Sony - PlayStation Portable"}, {L"XBOX", L"Microsoft - Xbox"},
        {L"X360", L"Microsoft - Xbox 360"}, {L"A2600", L"Atari - 2600"}, {L"ARCADE", L"FBNeo - Arcade Games"},
    };
    for (const auto& [id, folder] : folders) if (system == id) return folder;
    return L"";
}

std::wstring CoverKey(const std::wstring& name) {
    std::wstring text;
    int depth = 0;
    for (const wchar_t c : name) {
        if (c == L'(' || c == L'[') { ++depth; continue; }
        if (c == L')' || c == L']') { if (depth > 0) --depth; continue; }
        if (depth == 0) text += Plain(static_cast<wchar_t>(std::towlower(c)));
    }
    // "Legend of Zelda, The - ..." names the game "The Legend of Zelda - ...".
    for (size_t at; (at = text.find(L", the")) != std::wstring::npos;) text.erase(at, 5);
    std::wstring key;
    for (const wchar_t c : text) {
        if (std::iswalnum(c)) key += c;
        else if (!key.empty() && key.back() != L' ') key += L' ';
    }
    while (!key.empty() && key.back() == L' ') key.pop_back();
    if (key.rfind(L"the ", 0) == 0) key.erase(0, 4);
    return key;
}

std::wstring BestCover(const std::vector<std::wstring>& list, const std::wstring& name) {
    std::vector<Entry> entries;
    entries.reserve(list.size());
    for (const auto& entry : list) entries.push_back({CoverKey(entry), entry});
    return Best(entries, name);
}

CoverFetcher::CoverFetcher(std::wstring root) : root(std::move(root)) {
    worker = std::thread([this] { Work(); });
}

CoverFetcher::~CoverFetcher() {
    {
        std::lock_guard guard(lock);
        stop = true;
        jobs.clear();
    }
    wake.notify_all();
    worker.join();
}

void CoverFetcher::Request(const Game& game) {
    if (CoverFolder(game.system, Extension(game.folder)).empty()) return;
    {
        std::lock_guard guard(lock);
        if (!asked.insert(game.titleId).second) return;
    }
    if (Exists(BoxartFile(game)) || Exists(game.workspace + L"\\art\\boxart.miss")) return;
    {
        std::lock_guard guard(lock);
        jobs.push_back({game.titleId, game.system, game.workspace, game.folder, game.title});
    }
    wake.notify_one();
}

std::vector<std::wstring> CoverFetcher::TakeDone() {
    std::lock_guard guard(lock);
    return std::exchange(done, {});
}

void CoverFetcher::Work() {
    for (;;) {
        Job job;
        {
            std::unique_lock guard(lock);
            wake.wait(guard, [this] { return stop || !jobs.empty(); });
            if (stop) return;
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        Fetch(job);
    }
}

const std::vector<std::wstring>* CoverFetcher::List(const std::wstring& folder) {
    for (const auto& [name, list] : lists) if (name == folder) return &list;
    if (failedLists.count(folder) != 0) return nullptr;
    const std::wstring cache = root + L"\\covers\\" + folder + L".txt";
    std::vector<std::wstring> list;
    auto parse = [&](const std::string& text) {
        list.clear();
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            if (end > start) list.push_back(Wide(text.substr(start, end - start)));
            start = end + 1;
        }
    };
    // The list is fetched again a month after it was kept.
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const FILETIME kept = WriteTime(cache);
    const auto ticks = [](FILETIME t) { return (static_cast<unsigned long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    const bool fresh = Exists(cache) && ticks(now) - ticks(kept) < 30ull * 24 * 3600 * 10000000;
    if (fresh) parse(ReadBytes(cache));
    if (!fresh || list.empty()) {
        std::string page;
        if (Get(L"/" + Escape(folder) + L"/Named_Boxarts/", page) == 200) {
            std::string text;
            for (size_t at = 0; (at = page.find("href=\"", at)) != std::string::npos;) {
                at += 6;
                const size_t end = page.find('"', at);
                if (end == std::string::npos) break;
                const std::string href = page.substr(at, end - at);
                at = end;
                if (href.size() > 4 && href.compare(href.size() - 4, 4, ".png") == 0 && href.find('/') == std::string::npos)
                    text += Utf8(Unescape(href.substr(0, href.size() - 4))) + "\n";
            }
            if (!text.empty()) {
                CreateDirectoryW((root + L"\\covers").c_str(), nullptr);
                WriteAll(cache, text);
            }
            parse(text);
        } else if (Exists(cache)) {
            parse(ReadBytes(cache)); // an old list beats none while the server is out of reach
        }
    }
    if (list.empty()) {
        failedLists.insert(folder);
        return nullptr;
    }
    lists.emplace_back(folder, std::move(list));
    return &lists.back().second;
}

void CoverFetcher::Fetch(const Job& job) {
    const std::wstring folder = CoverFolder(job.system, Extension(job.folder));
    const std::vector<std::wstring>* list = List(folder);
    if (list == nullptr) return; // offline or the server has no list: try again next time

    // Keys of the list, made once per console.
    static thread_local std::wstring keyedFolder;
    static thread_local std::vector<Entry> entries;
    if (keyedFolder != folder) {
        entries.clear();
        entries.reserve(list->size());
        for (const auto& name : *list) entries.push_back({CoverKey(name), name});
        keyedFolder = folder;
    }

    // The library's title first, then the file or folder name, which for a dump usually is the set's own name.
    std::wstring stem = FileName(job.folder);
    if (!IsDirectory(job.folder) && stem.find_last_of(L'.') != std::wstring::npos) stem.resize(stem.find_last_of(L'.'));
    std::wstring match;
    for (const std::wstring& name : {job.title, stem}) {
        if (match.empty() && !name.empty()) match = Best(entries, name);
    }

    const std::wstring art = job.workspace + L"\\art";
    CreateDirectoryW((root + L"\\games").c_str(), nullptr);
    CreateDirectoryW(job.workspace.c_str(), nullptr);
    CreateDirectoryW(art.c_str(), nullptr);
    if (match.empty()) {
        WriteAll(art + L"\\boxart.miss", Utf8(folder) + "\n");
        return;
    }
    std::string png;
    const int status = Get(L"/" + Escape(folder) + L"/Named_Boxarts/" + Escape(match) + L".png", png);
    if (status == 404) {
        WriteAll(art + L"\\boxart.miss", Utf8(folder + L"/" + match) + "\n");
        return;
    }
    if (status != 200 || png.size() < 8 || png.compare(1, 3, "PNG") != 0) return;
    if (!WriteAll(art + L"\\boxart.png", png)) return;
    std::lock_guard guard(lock);
    done.push_back(job.titleId);
}

}
