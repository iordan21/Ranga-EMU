#include "Idioma.hpp"

#include "Common.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace Idioma {
using namespace Ranga;
namespace {

constexpr const char* kCodes[Count] = {"pt", "en", "ru", "es"};
constexpr const char* kNames[Count] = {"Português", "English", "Русский", "Español"};

Lang g_lang = Pt;
// Portuguese -> the three others.
std::unordered_map<std::string, std::string> g_table[Count];
// The texts that are pieces of a longer one (begin or end with a space or a sign): tried inside a text, longest first.
std::vector<std::pair<std::string, const std::string*>> g_pieces;
// What was already translated (texts that are not in the table included): unordered_map keeps the strings in place,
// so the pointers handed out stay good until NewFrame clears it.
std::unordered_map<std::string, std::string> g_done;
std::unordered_map<std::string, std::string> g_labels;

std::string Unescape(const std::string& line) {
    std::string out;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] != '\\' || i + 1 == line.size()) {
            out += line[i];
            continue;
        }
        const char c = line[++i];
        out += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c;
    }
    return out;
}

bool IsPiece(const std::string& pt) {
    if (pt.size() < 5) return false;
    const auto edge = [](char c) { return c == ' ' || c == '(' || c == ')' || c == ':' || c == ',' || c == '.' || c == '-' || c == '/'; };
    return edge(pt.front()) || edge(pt.back());
}

void LoadTable() {
    HRSRC found = FindResourceW(nullptr, L"IDIOMAS", reinterpret_cast<LPCWSTR>(RT_RCDATA));
    HGLOBAL loaded = found != nullptr ? LoadResource(nullptr, found) : nullptr;
    if (loaded == nullptr) return;
    const char* data = static_cast<const char*>(LockResource(loaded));
    const std::string text(data, SizeofResource(nullptr, found));
    std::vector<std::string> block;
    const auto flush = [&] {
        if (block.size() >= 4) {
            const std::string pt = Unescape(block[0]);
            for (int lang = En; lang < Count; ++lang) {
                const std::string out = Unescape(block[lang]);
                if (!out.empty() && out != pt) g_table[lang][pt] = out;
            }
        }
        block.clear();
    };
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) flush();
        else if (line[0] != '#' || !block.empty()) block.push_back(line);
    }
    flush();
}

void LoadPieces() {
    g_pieces.clear();
    if (g_lang == Pt) return;
    for (const auto& [pt, out] : g_table[g_lang])
        if (IsPiece(pt)) g_pieces.emplace_back(pt, &out);
    std::sort(g_pieces.begin(), g_pieces.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
}

Lang FromCode(const std::wstring& code) {
    for (int i = 0; i < Count; ++i)
        if (code.size() >= 2 && towlower(code[0]) == kCodes[i][0] && towlower(code[1]) == kCodes[i][1]) return static_cast<Lang>(i);
    return Count;
}

Lang FromWindows() {
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_PORTUGUESE: return Pt;
    case LANG_RUSSIAN: return Ru;
    case LANG_SPANISH: return Es;
    default: return En;
    }
}

// The pieces of the table found inside text, each swapped once (a swapped part is not looked at again).
// Text made of parts joined by " · " ("Sony · 2006 · Nenhum jogo ainda"): each part that is in the table, swapped.
std::string SwapParts(const std::string& text) {
    static const std::string dot = "·"; // ·
    if (text.find(dot) == std::string::npos) return text;
    const auto& table = g_table[g_lang];
    std::string out;
    size_t start = 0;
    while (true) {
        const size_t end = text.find(dot, start);
        const std::string part = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const size_t first = part.find_first_not_of(' '), last = part.find_last_not_of(' ');
        if (first == std::string::npos) out += part;
        else {
            const auto found = table.find(part.substr(first, last - first + 1));
            out += found == table.end() ? part : part.substr(0, first) + found->second + part.substr(last + 1);
        }
        if (end == std::string::npos) break;
        out += dot;
        start = end + dot.size();
    }
    return out;
}

std::string SwapPieces(const std::string& text) {
    // Only short text: the pieces are of status lines; inside prose (notes, descriptions) they would mix languages.
    std::string out = SwapParts(text);
    if (g_pieces.empty() || text.size() > 140) return out;
    std::vector<std::pair<size_t, size_t>> done; // ranges of out already translated
    for (const auto& [pt, translated] : g_pieces) {
        size_t at = 0;
        while ((at = out.find(pt, at)) != std::string::npos) {
            const bool overlaps = std::any_of(done.begin(), done.end(), [&](const auto& r) { return at < r.second && at + pt.size() > r.first; });
            if (overlaps) {
                at += pt.size();
                continue;
            }
            out.replace(at, pt.size(), *translated);
            const long long grow = static_cast<long long>(translated->size()) - static_cast<long long>(pt.size());
            for (auto& r : done) {
                if (r.first >= at + pt.size()) {
                    r.first += grow;
                    r.second += grow;
                }
            }
            done.emplace_back(at, at + translated->size());
            at += translated->size();
        }
    }
    return out;
}

}

void Init(const std::wstring& root) {
    LoadTable();
    const Lang saved = FromCode(ReadIni(root + L"\\engines.ini", L"Ranga", L"idioma", L""));
    g_lang = saved != Count ? saved : FromWindows();
    LoadPieces();
}

Lang Current() { return g_lang; }

void Set(const std::wstring& root, Lang lang) {
    if (lang < 0 || lang >= Count) return;
    g_lang = lang;
    WriteIni(L"Ranga", L"idioma", Wide(kCodes[lang]).c_str(), (root + L"\\engines.ini").c_str());
    g_done.clear();
    g_labels.clear();
    LoadPieces();
}

const char* Code(Lang lang) { return kCodes[lang]; }
const char* Name(Lang lang) { return kNames[lang]; }

const char* Tr(const char* pt) {
    if (g_lang == Pt || pt == nullptr || *pt == 0) return pt;
    const auto& table = g_table[g_lang];
    if (auto exact = table.find(pt); exact != table.end()) return exact->second.c_str();
    auto [entry, fresh] = g_done.try_emplace(pt);
    if (fresh) entry->second = SwapPieces(entry->first);
    return entry->second == entry->first ? pt : entry->second.c_str();
}

std::string Tr(const std::string& pt) { return g_lang == Pt ? pt : std::string(Tr(pt.c_str())); }

std::wstring TrW(const std::wstring& pt) { return g_lang == Pt ? pt : Wide(Tr(Utf8(pt))); }

const char* Label(const char* label) {
    if (g_lang == Pt || label == nullptr) return label;
    const char* hash = std::strstr(label, "##");
    if (hash == label) return label; // nothing shown
    auto [entry, fresh] = g_labels.try_emplace(label);
    if (fresh) {
        const std::string shown = hash != nullptr ? std::string(label, hash) : std::string(label);
        const std::string translated = Tr(shown);
        if (translated == shown) entry->second = label;
        else if (hash != nullptr && std::strncmp(hash, "###", 3) == 0) entry->second = translated + hash;
        else entry->second = translated + "###" + label;
    }
    return entry->second.c_str();
}

std::string Format(const char* ptFormat, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, ptFormat);
    vsnprintf(buffer, sizeof(buffer), Tr(ptFormat), args);
    va_end(args);
    return buffer;
}

void NewFrame() {
    if (g_done.size() > 4000) g_done.clear();
    if (g_labels.size() > 4000) g_labels.clear();
}

}
