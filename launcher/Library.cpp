#include "Library.hpp"

#include <algorithm>
#include <cstdio>
#include <cwchar>

namespace Ranga {

namespace {

constexpr const wchar_t* kSonsOfSparta = L"PPSA28997";

// The string value of the first "key" at or after from, with the JSON escapes param.json uses; empty when there is none.
std::string JsonString(const std::string& text, const std::string& key, size_t from = 0) {
    size_t at = text.find("\"" + key + "\"", from);
    if (at == std::string::npos) return {};
    at = text.find(':', at + key.size() + 2);
    if (at == std::string::npos) return {};
    at = text.find_first_not_of(" \t\r\n", at + 1);
    if (at == std::string::npos || text[at] != '"') return {};
    std::string value;
    for (size_t i = at + 1; i < text.size(); ++i) {
        const char character = text[i];
        if (character == '"') return value;
        if (character != '\\' || i + 1 >= text.size()) {
            value += character;
            continue;
        }
        const char escaped = text[++i];
        if (escaped == 'u' && i + 4 < text.size()) {
            const std::wstring unit(1, static_cast<wchar_t>(std::stoul(text.substr(i + 1, 4), nullptr, 16)));
            value += Utf8(unit);
            i += 4;
        } else {
            value += escaped == 'n' ? '\n' : escaped == 't' ? '\t' : escaped;
        }
    }
    return {};
}

// The profile API writes Unicode only into a file that already starts with a UTF-16 byte order mark.
void EnsureUnicodeIni(const std::wstring& path) {
    if (Exists(path)) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) return;
    const unsigned char bom[2] = {0xFF, 0xFE};
    fwrite(bom, 1, sizeof(bom), file);
    fclose(file);
}

std::wstring ReadIni(const std::wstring& path, const wchar_t* section, const wchar_t* key, const wchar_t* fallback) {
    wchar_t value[MAX_PATH * 2] = {};
    GetPrivateProfileStringW(section, key, fallback, value, MAX_PATH * 2, path.c_str());
    return value;
}

bool ReadFlag(const std::wstring& path, const wchar_t* section, const wchar_t* key) { return ReadIni(path, section, key, L"0") == L"1"; }

void WriteFlag(const std::wstring& path, const wchar_t* section, const wchar_t* key, bool value) {
    WritePrivateProfileStringW(section, key, value ? L"1" : L"0", path.c_str());
}

}

Profile ProfileFor(const std::wstring& titleId) {
    Profile profile;
    // Unity plugins and a network library the runtime replaces; deferred modules start when the game first needs them.
    profile.excludedModules = {L"CommonDialog.prx", L"VoiceInputPlugin.prx", L"libSceNpCppWebApi.prx"};
    profile.deferredModules = {L"PSN.prx", L"SaveData.prx", L"lib_burst_generated.prx"};
    if (titleId == kSonsOfSparta) {
        profile.tested = true;
        profile.sonsOfSparta = true;
    }
    if (titleId == L"PPSA01342") {
        // Demon's Souls: the GPU reads the game's memory in place instead of from copies (1-2 FPS without it). On
        // NVIDIA every buffer the GPU maps from system memory, imports included, shares a heap of half the RAM, and
        // the menu alone imports about 7.3 GB, so the budget leaves the driver to say when that heap is full (idle
        // imports are given back then) and the emulator keeps few spare host buffers in it.
        MEMORYSTATUSEX memory{sizeof(memory)};
        GlobalMemoryStatusEx(&memory);
        const auto importMib = std::max<unsigned long long>(4096, memory.ullTotalPhys / (1024 * 1024) * 7 / 10);
        profile.environment = {{L"APS5_HOST_IMPORT", L"1"}, {L"APS5_HOST_IMPORT_MIB", std::to_wstring(importMib)}, {L"APS5_HOST_POOL_MIB", L"96"}};
    }
    return profile;
}

bool ReadGameInfo(const std::wstring& folder, Game& game) {
    const std::string text = ReadBytes(folder + L"\\sce_sys\\param.json");
    const std::string titleId = JsonString(text, "titleId");
    if (titleId.empty()) return false;
    std::string title;
    const std::string language = JsonString(text, "defaultLanguage");
    const size_t localized = text.find("\"localizedParameters\"");
    if (!language.empty() && localized != std::string::npos) {
        const size_t block = text.find("\"" + language + "\"", localized);
        if (block != std::string::npos) title = JsonString(text, "titleName", block);
    }
    if (title.empty()) title = JsonString(text, "titleName");
    game.titleId = Wide(titleId);
    game.title = title.empty() ? game.titleId : Wide(title);
    game.version = Wide(JsonString(text, "contentVersion"));
    game.folder = folder;
    return true;
}

Library::Library(std::wstring root) : root(std::move(root)) {}

void Library::Load() {
    games.clear();
    std::vector<wchar_t> names(32768);
    const DWORD length = GetPrivateProfileSectionNamesW(names.data(), static_cast<DWORD>(names.size()), IniPath().c_str());
    for (const wchar_t* name = names.data(); name < names.data() + length && *name != L'\0'; name += wcslen(name) + 1) {
        const std::wstring titleId = name;
        if (titleId == L"Client") continue;
        Game game;
        const std::wstring folder = ReadIni(IniPath(), name, L"folder", L"");
        // A game whose folder is gone stays listed under its stored name, so it can be pointed at its new place.
        if (!ReadGameInfo(folder, game) || game.titleId != titleId) {
            game = Game{};
            game.titleId = titleId;
            game.title = ReadIni(IniPath(), name, L"title", name);
            game.folder = folder;
        }
        game.workspace = root + L"\\games\\" + titleId;
        games.push_back(game);
    }
    std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) { return _wcsicmp(a.title.c_str(), b.title.c_str()) < 0; });
}

int Library::Add(const std::wstring& folder, std::wstring& error) {
    Game game;
    if (!IsGameFolder(folder)) {
        error = L"Esta pasta não tem os arquivos do jogo:\n\n" + folder + L"\n\nEscolha a pasta que tem eboot.bin (ou eboot.elf) e sce_sys.";
        return -1;
    }
    if (!ReadGameInfo(folder, game)) {
        error = L"Não deu para ler sce_sys\\param.json em:\n\n" + folder;
        return -1;
    }
    EnsureUnicodeIni(IniPath());
    if (!WritePrivateProfileStringW(game.titleId.c_str(), L"folder", folder.c_str(), IniPath().c_str()) ||
        !WritePrivateProfileStringW(game.titleId.c_str(), L"title", game.title.c_str(), IniPath().c_str())) {
        error = L"Não deu para salvar library.ini em:\n\n" + root;
        return -1;
    }
    Load();
    for (size_t i = 0; i < games.size(); ++i) {
        if (games[i].titleId == game.titleId) return static_cast<int>(i);
    }
    return -1;
}

void Library::Remove(size_t index) {
    if (index >= games.size()) return;
    WritePrivateProfileStringW(games[index].titleId.c_str(), nullptr, nullptr, IniPath().c_str());
    Load();
}

std::wstring Library::Selected() const { return ReadIni(IniPath(), L"Client", L"selected", L""); }

void Library::Select(const std::wstring& titleId) {
    EnsureUnicodeIni(IniPath());
    WritePrivateProfileStringW(L"Client", L"selected", titleId.c_str(), IniPath().c_str());
}

// The keys are the ones the single-game launcher used in launcher.ini, so its file carries over as options.ini.
Options LoadOptions(const Game& game) {
    const std::wstring path = game.workspace + L"\\options.ini";
    Options options;
    options.borderless = _wcsicmp(ReadIni(path, L"Display", L"mode", L"Windowed").c_str(), L"Borderless") == 0;
    const std::wstring resolution = ReadIni(path, L"Display", L"resolution", L"");
    options.resolution = !resolution.empty() ? _wtoi(resolution.c_str()) : ProfileFor(game.titleId).sonsOfSparta ? ReadSosSetting(game.workspace, "resolutionIndex") : -1;
    if (options.resolution != 0 && options.resolution != 6 && options.resolution != 8) options.resolution = -1;
    options.skipIntro = ReadFlag(path, L"Startup", L"skip_intro");
    options.skipDialogue = ReadFlag(path, L"QualityOfLife", L"SkipDialogue");
    options.godMode = ReadFlag(path, L"Cheats", L"god_mode");
    options.infiniteSpirit = ReadFlag(path, L"Cheats", L"infinite_spartan_spirit");
    options.infiniteMagic = ReadFlag(path, L"Cheats", L"infinite_magic");
    options.movementSpeed = ReadFlag(path, L"Cheats", L"movement_speed");
    options.jumpHeight = ReadFlag(path, L"Cheats", L"jump_height");
    const int damage = _wtoi(ReadIni(path, L"Cheats", L"damage_multiplier", L"1").c_str());
    options.damageMultiplier = damage == 2 || damage == 4 || damage == 6 ? damage : 1;
    return options;
}

namespace {

std::wstring AddonIni(const Game& game) { return game.workspace + L"\\dlc.ini"; }

// The runtime reads one entitlement label per line; the file is rewritten from dlc.ini so the two always agree.
void WriteEntitlements(const Game& game, const std::vector<Addon>& addons) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, (game.workspace + L"\\anyps5-entitlements.ini").c_str(), L"wb") != 0 || file == nullptr) return;
    std::fputs("# Add-ons installed with Ranga Emu, one entitlement label per line. Edit them in the DLC tab.\r\n", file);
    for (const auto& addon : addons) std::fprintf(file, "%s\r\n", Utf8(addon.label).c_str());
    fclose(file);
}

bool HasDataBesideSceSys(const std::wstring& folder) {
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((folder + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return false;
    bool data = false;
    do {
        const std::wstring name = found.cFileName;
        if (name != L"." && name != L".." && _wcsicmp(name.c_str(), L"sce_sys") != 0) data = true;
    } while (!data && FindNextFileW(search, &found));
    FindClose(search);
    return data;
}

}

std::vector<Addon> LoadAddons(const Game& game) {
    std::vector<Addon> addons;
    std::vector<wchar_t> names(32768);
    const DWORD length = GetPrivateProfileSectionNamesW(names.data(), static_cast<DWORD>(names.size()), AddonIni(game).c_str());
    for (const wchar_t* name = names.data(); name < names.data() + length && *name != L'\0'; name += wcslen(name) + 1) {
        Addon addon;
        addon.label = name;
        addon.title = ReadIni(AddonIni(game), name, L"title", name);
        addon.folder = ReadIni(AddonIni(game), name, L"folder", L"");
        addon.hasData = ReadFlag(AddonIni(game), name, L"data");
        addons.push_back(addon);
    }
    return addons;
}

std::wstring InstallAddon(const Game& game, const std::wstring& folder) {
    const std::string text = ReadBytes(folder + L"\\sce_sys\\param.json");
    if (text.empty()) return L"Esta pasta não tem sce_sys\\param.json, então não parece uma DLC tirada do console:\n\n" + folder;
    const std::wstring titleId = Wide(JsonString(text, "titleId"));
    const std::wstring contentId = Wide(JsonString(text, "contentId"));
    // A content id is <service>-<title id>_<nn>-<entitlement label>, and the label has 16 characters.
    const size_t dash = contentId.find_last_of(L'-');
    const std::wstring label = dash == std::wstring::npos ? L"" : contentId.substr(dash + 1);
    if (label.size() != 16 || label.find_first_of(L"/\\.") != std::wstring::npos) return L"Não deu para ler o código da DLC (contentId) no param.json:\n\n" + folder;
    if (!titleId.empty() && titleId != game.titleId) return L"Esta DLC é de outro jogo (" + titleId + L"), não de " + game.title + L" (" + game.titleId + L").";
    if (IsGameFolder(folder)) return L"Esta pasta é um jogo, não uma DLC. Use \"Adicionar jogo\" para ela.";
    std::string title;
    const std::string language = JsonString(text, "defaultLanguage");
    const size_t localized = text.find("\"localizedParameters\"");
    if (!language.empty() && localized != std::string::npos) {
        const size_t block = text.find("\"" + language + "\"", localized);
        if (block != std::string::npos) title = JsonString(text, "titleName", block);
    }
    if (title.empty()) title = JsonString(text, "titleName");
    const bool data = HasDataBesideSceSys(folder);
    CreateDirectoryW((game.workspace.substr(0, game.workspace.find_last_of(L'\\'))).c_str(), nullptr);
    CreateDirectoryW(game.workspace.c_str(), nullptr);
    EnsureUnicodeIni(AddonIni(game));
    const std::wstring ini = AddonIni(game);
    if (!WritePrivateProfileStringW(label.c_str(), L"folder", folder.c_str(), ini.c_str()) ||
        !WritePrivateProfileStringW(label.c_str(), L"title", (title.empty() ? label : Wide(title)).c_str(), ini.c_str())) {
        return L"Não deu para salvar dlc.ini em:\n\n" + game.workspace;
    }
    WriteFlag(ini, label.c_str(), L"data", data);
    if (data) {
        CreateDirectoryW((game.workspace + L"\\logs").c_str(), nullptr);
        LinkFolder(game.workspace + L"\\addcont\\" + label, folder, game.workspace + L"\\logs\\link.log");
        if (FinalPath(game.workspace + L"\\addcont\\" + label) != FinalPath(folder)) {
            WritePrivateProfileStringW(label.c_str(), nullptr, nullptr, ini.c_str());
            return L"Não deu para ligar a pasta da DLC em addcont. Veja link.log na aba Log.";
        }
    }
    WriteEntitlements(game, LoadAddons(game));
    return L"";
}

void RemoveAddon(const Game& game, const std::wstring& label) {
    // Only the junction goes; the add-on's own folder is left alone.
    RemoveDirectoryW((game.workspace + L"\\addcont\\" + label).c_str());
    WritePrivateProfileStringW(label.c_str(), nullptr, nullptr, AddonIni(game).c_str());
    WriteEntitlements(game, LoadAddons(game));
}

void SaveOptions(const Game& game, const Options& options) {
    CreateDirectoryW((game.workspace.substr(0, game.workspace.find_last_of(L'\\'))).c_str(), nullptr);
    CreateDirectoryW(game.workspace.c_str(), nullptr);
    const std::wstring path = game.workspace + L"\\options.ini";
    WritePrivateProfileStringW(L"Display", L"mode", options.borderless ? L"Borderless" : L"Windowed", path.c_str());
    WritePrivateProfileStringW(L"Display", L"resolution", std::to_wstring(options.resolution).c_str(), path.c_str());
    WriteFlag(path, L"Startup", L"skip_intro", options.skipIntro);
    WriteFlag(path, L"QualityOfLife", L"SkipDialogue", options.skipDialogue);
    WriteFlag(path, L"Cheats", L"god_mode", options.godMode);
    WriteFlag(path, L"Cheats", L"infinite_spartan_spirit", options.infiniteSpirit);
    WriteFlag(path, L"Cheats", L"infinite_magic", options.infiniteMagic);
    WriteFlag(path, L"Cheats", L"movement_speed", options.movementSpeed);
    WriteFlag(path, L"Cheats", L"jump_height", options.jumpHeight);
    WritePrivateProfileStringW(L"Cheats", L"damage_multiplier", options.damageMultiplier == 1 ? nullptr : std::to_wstring(options.damageMultiplier).c_str(), path.c_str());
}

}
