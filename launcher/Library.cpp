#include "Library.hpp"
#include "Systems.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <map>

namespace Ranga {

std::wstring WorkspaceFor(const std::wstring& root, const std::wstring& system, const std::wstring& title) {
    // What a Windows folder name cannot have goes; a name ending in a dot or space is trimmed, and a long one cut.
    const auto clean = [](std::wstring name) {
        std::wstring out;
        for (const wchar_t c : name) {
            if (c < 32 || wcschr(L"\\/:*?\"<>|", c) != nullptr) out += c == L':' ? L" -" : L"";
            else out += c;
        }
        while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) out.pop_back();
        while (!out.empty() && out.front() == L' ') out.erase(0, 1);
        if (out.size() > 80) out.resize(80);
        while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) out.pop_back();
        return out.empty() ? std::wstring(L"jogo") : out;
    };
    return root + L"\\saves\\" + clean(Wide(SystemName(system))) + L"\\" + clean(title);
}

void MakeFolders(const std::wstring& path) {
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path), error);
}

namespace {

constexpr const wchar_t* kSonsOfSparta = L"PPSA28997";
constexpr const wchar_t* kBloodborne = L"CUSA03173";

bool ReadFlag(const std::wstring& path, const wchar_t* section, const wchar_t* key) { return ReadIni(path, section, key, L"0") == L"1"; }

void WriteFlag(const std::wstring& path, const wchar_t* section, const wchar_t* key, bool value) {
    WritePrivateProfileStringW(section, key, value ? L"1" : L"0", path.c_str());
}

}

// The string value of the first "key" at or after from, with the JSON escapes param.json uses; empty when there is none.
std::string JsonString(const std::string& text, const std::string& key, size_t from) {
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

Profile ProfileFor(const std::wstring& titleId) {
    Profile profile;
    // Unity plugins and a network library the runtime replaces; deferred modules start when the game first needs them.
    profile.excludedModules = {L"CommonDialog.prx", L"VoiceInputPlugin.prx", L"libSceNpCppWebApi.prx"};
    profile.deferredModules = {L"PSN.prx", L"SaveData.prx", L"lib_burst_generated.prx"};
    if (titleId == kSonsOfSparta) {
        profile.tested = true;
        profile.sonsOfSparta = true;
    }
    if (titleId == kBloodborne) profile.bloodborne = true;
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

std::wstring BloodborneEngine(const std::wstring& root) { return root + L"\\engines\\bloodborne"; }

bool ReadGameInfo(const std::wstring& folder, Game& game) {
    const std::string text = ReadBytes(folder + L"\\sce_sys\\param.json");
    const std::string titleId = JsonString(text, "titleId");
    if (titleId.empty()) {
        // A PS4 game: the title in param.sfo, the version of its merged update in APP_VER.
        const std::string sfo = ReadBytes(folder + L"\\sce_sys\\param.sfo");
        const std::string ps4Id = SfoString(sfo, "TITLE_ID");
        if (ps4Id.empty()) return false;
        const std::string title = SfoString(sfo, "TITLE");
        game.titleId = Wide(ps4Id);
        game.title = title.empty() ? game.titleId : Wide(title);
        game.version = Wide(SfoString(sfo, "APP_VER"));
        game.folder = folder;
        game.system = L"PS4";
        return true;
    }
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
    game.system = L"PS5";
    return true;
}

bool GameAvailable(const Game& game) {
    if (game.system == L"PS5" || (game.system == L"PS4" && ProfileFor(game.titleId).bloodborne)) return IsGameFolder(game.folder);
    return !GameRom(game.system, game.folder).empty();
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
        // Entries from before other consoles have no system: their title id says PS4 or PS5.
        const std::wstring system = ReadIni(IniPath(), name, L"system", titleId.rfind(L"CUSA", 0) == 0 ? L"PS4" : L"PS5");
        // A game whose folder is gone stays listed under its stored name, so it can be pointed at its new place. Other
        // consoles keep what was read when they were added.
        if (system != L"PS5" && system != L"PS4") {
            game.titleId = titleId;
            game.title = ReadIni(IniPath(), name, L"title", name);
            game.folder = folder;
            game.system = system;
            game.serial = ReadIni(IniPath(), name, L"serial", L"");
        } else if (!ReadGameInfo(folder, game) || game.titleId != titleId) {
            game = Game{};
            game.titleId = titleId;
            game.title = ReadIni(IniPath(), name, L"title", name);
            game.folder = folder;
            game.system = system;
        }
        games.push_back(game);
    }
    // Each game's folder by console and title; titles two games of a console share get the game's key as well, on
    // both, so which one is which never depends on the order the list is read in.
    std::map<std::wstring, int> taken;
    for (const auto& game : games) ++taken[Lower(WorkspaceFor(root, game.system, game.title))];
    for (auto& game : games) {
        game.workspace = WorkspaceFor(root, game.system, game.title);
        if (taken[Lower(game.workspace)] > 1) game.workspace += L" [" + game.titleId + L"]";
    }
    std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) { return _wcsicmp(a.title.c_str(), b.title.c_str()) < 0; });
}

int Library::Add(const std::wstring& folder, std::wstring& error) {
    Game game;
    if (!IsGameFolder(folder)) {
        // Not a PS5 or PS4 dump: maybe a PS3, Xbox 360 or Wii U game folder.
        const Detected found = DetectFolder(folder);
        if (found.systems.size() == 1) {
            const std::wstring key = found.systems[0] == L"PS3" && !found.serial.empty() ? found.serial : FileKey(found.systems[0], folder);
            return Store(key, folder, found.systems[0], found.serial, found.title, error);
        }
        error = L"Não reconheci o jogo nesta pasta:\n\n" + folder +
                L"\n\nPasta de jogo é a de PS5 ou do Bloodborne (com eboot.bin e sce_sys), PS3 (com PS3_GAME ou USRDIR), Xbox 360 (com default.xex) ou Wii U (com code e meta). Para ISO, ROM e outros arquivos, use \"Arquivo do jogo\".";
        return -1;
    }
    if (!ReadGameInfo(folder, game)) {
        error = L"Não deu para ler sce_sys\\param.json (PS5) nem sce_sys\\param.sfo (PS4) em:\n\n" + folder;
        return -1;
    }
    // PS4 is not one of Ranga's consoles: only Bloodborne runs, through its port, as an experiment.
    if (game.system == L"PS4" && !ProfileFor(game.titleId).bloodborne) {
        error = L"O Ranga não roda jogos de PS4: " + game.title + L" (" + game.titleId +
                L") ficou de fora. Do PS4, só o Bloodborne (CUSA03173), como experimento, pelo port dele.";
        return -1;
    }
    return Store(game.titleId, folder, game.system, L"", game.title, error);
}

int Library::AddFile(const std::wstring& path, const std::wstring& system, const std::wstring& serial, const std::wstring& title, std::wstring& error) {
    const std::wstring key = FileKey(system, path);
    const int index = Store(key, path, system, serial, title, error);
    if (index >= 0) ExtractArt(system, path, games[static_cast<size_t>(index)].workspace + L"\\art");
    return index;
}

int Library::Store(const std::wstring& key, const std::wstring& path, const std::wstring& system, const std::wstring& serial, const std::wstring& title, std::wstring& error) {
    EnsureUnicodeIni(IniPath());
    if (!WritePrivateProfileStringW(key.c_str(), L"folder", path.c_str(), IniPath().c_str()) ||
        !WritePrivateProfileStringW(key.c_str(), L"title", title.c_str(), IniPath().c_str()) ||
        !WritePrivateProfileStringW(key.c_str(), L"system", system.c_str(), IniPath().c_str())) {
        error = L"Não deu para salvar library.ini em:\n\n" + root;
        return -1;
    }
    WritePrivateProfileStringW(key.c_str(), L"serial", serial.empty() ? nullptr : serial.c_str(), IniPath().c_str());
    Load();
    for (size_t i = 0; i < games.size(); ++i) {
        if (games[i].titleId == key) return static_cast<int>(i);
    }
    return -1;
}

// The art inside a disc image goes in the game's folder, which is known once the library is read again.
void Library::ExtractArts(const std::vector<PendingArt>& arts) {
    for (const auto& art : arts) {
        for (const auto& game : games) {
            if (game.titleId == art.key) ExtractArt(art.system, art.file, game.workspace + L"\\art");
        }
    }
}

void Library::AddRomFolder(const std::wstring& folder, int& added, int& ambiguous, int& unknown) {
    added = ambiguous = unknown = 0;
    std::vector<PendingArt> arts;
    std::vector<std::wstring> files;
    const auto list = [&](const std::wstring& directory, bool descend) {
        WIN32_FIND_DATAW found;
        HANDLE search = FindFirstFileW((directory + L"\\*").c_str(), &found);
        if (search == INVALID_HANDLE_VALUE) return std::vector<std::wstring>{};
        std::vector<std::wstring> folders;
        do {
            const std::wstring name = found.cFileName;
            if (name == L"." || name == L"..") continue;
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                if (descend) folders.push_back(directory + L"\\" + name);
            } else {
                files.push_back(directory + L"\\" + name);
            }
        } while (FindNextFileW(search, &found));
        FindClose(search);
        return folders;
    };
    for (const auto& inner : list(folder, true)) list(inner, false);
    // A track named by a cue sheet beside it is part of that disc, not a game of its own.
    std::vector<std::wstring> sheets;
    for (const auto& file : files) {
        if (Extension(file) == L".cue" || Extension(file) == L".m3u") sheets.push_back(Lower(file.substr(0, file.find_last_of(L'.'))));
    }
    EnsureUnicodeIni(IniPath());
    for (const auto& file : files) {
        const std::wstring extension = Extension(file);
        if ((extension == L".bin" || extension == L".img") &&
            std::find(sheets.begin(), sheets.end(), Lower(file.substr(0, file.find_last_of(L'.')))) != sheets.end())
            continue;
        const Detected found = DetectFile(file);
        if (found.systems.empty()) {
            ++unknown;
            continue;
        }
        if (found.systems.size() > 1) {
            ++ambiguous;
            continue;
        }
        const std::wstring key = FileKey(found.systems[0], file);
        WritePrivateProfileStringW(key.c_str(), L"folder", file.c_str(), IniPath().c_str());
        WritePrivateProfileStringW(key.c_str(), L"title", found.title.c_str(), IniPath().c_str());
        WritePrivateProfileStringW(key.c_str(), L"system", found.systems[0].c_str(), IniPath().c_str());
        if (!found.serial.empty()) WritePrivateProfileStringW(key.c_str(), L"serial", found.serial.c_str(), IniPath().c_str());
        arts.push_back({found.systems[0], file, key});
        ++added;
    }
    Load();
    ExtractArts(arts);
}

void Library::Relocate(const std::wstring& titleId, const std::wstring& path) {
    WritePrivateProfileStringW(titleId.c_str(), L"folder", path.c_str(), IniPath().c_str());
    Load();
}

void Library::Remove(size_t index) {
    if (index >= games.size()) return;
    WritePrivateProfileStringW(games[index].titleId.c_str(), nullptr, nullptr, IniPath().c_str());
    Load();
}

std::wstring Library::GamesFolder(const std::wstring& system) const {
    std::wstring name = Wide(SystemName(system));
    for (auto& character : name) if (wcschr(L"\\/:*?\"<>|", character) != nullptr) character = L'-';
    return root + L"\\jogos\\" + name;
}

int Library::ScanGamesFolders(const std::wstring& only) {
    const auto list = [](const std::wstring& directory, std::vector<std::wstring>& files, std::vector<std::wstring>& folders) {
        WIN32_FIND_DATAW found;
        HANDLE search = FindFirstFileW((directory + L"\\*").c_str(), &found);
        if (search == INVALID_HANDLE_VALUE) return;
        do {
            const std::wstring name = found.cFileName;
            if (name == L"." || name == L"..") continue;
            ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? folders : files).push_back(directory + L"\\" + name);
        } while (FindNextFileW(search, &found));
        FindClose(search);
    };
    std::vector<PendingArt> arts;
    const auto known = [this](const std::wstring& key) { return !ReadIni(IniPath(), key.c_str(), L"folder", L"").empty(); };
    const auto store = [this](const std::wstring& key, const std::wstring& path, const std::wstring& system, const std::wstring& serial, const std::wstring& title) {
        WritePrivateProfileStringW(key.c_str(), L"folder", path.c_str(), IniPath().c_str());
        WritePrivateProfileStringW(key.c_str(), L"title", title.c_str(), IniPath().c_str());
        WritePrivateProfileStringW(key.c_str(), L"system", system.c_str(), IniPath().c_str());
        if (!serial.empty()) WritePrivateProfileStringW(key.c_str(), L"serial", serial.c_str(), IniPath().c_str());
    };
    std::vector<std::wstring> ids;
    if (only.empty()) {
        if (ShowPs4Ps5(root)) ids.push_back(L"PS5");
        for (const auto& system : Systems()) ids.push_back(system.id);
    } else {
        ids.push_back(only);
    }
    CreateDirectoryW((root + L"\\jogos").c_str(), nullptr);
    EnsureUnicodeIni(IniPath());
    int added = 0;
    // Switch updates and add-ons found on the way, for the games they belong to.
    std::vector<std::pair<std::wstring, SwitchContent>> extras;
    for (const auto& id : ids) {
        const std::wstring folder = GamesFolder(id);
        CreateDirectoryW(folder.c_str(), nullptr);
        std::vector<std::wstring> files, folders;
        list(folder, files, folders);
        const System* system = FindSystem(id);
        for (const auto& inner : folders) {
            if (id == L"PS5") {
                Game game;
                if (IsGameFolder(inner) && ReadGameInfo(inner, game) && game.system == L"PS5" && !known(game.titleId)) {
                    store(game.titleId, inner, L"PS5", L"", game.title);
                    ++added;
                }
                continue;
            }
            if (system != nullptr && system->folders) {
                const Detected found = DetectFolder(inner);
                if (std::find(found.systems.begin(), found.systems.end(), id) != found.systems.end()) {
                    const std::wstring key = id == L"PS3" && !found.serial.empty() ? found.serial : FileKey(id, inner);
                    if (!known(key)) {
                        store(key, inner, id, found.serial, found.title);
                        ++added;
                    }
                    continue;
                }
            }
            // Any other folder is one the user made to keep a game's files together.
            std::vector<std::wstring> ignored;
            list(inner, files, ignored);
        }
        if (system == nullptr) continue;
        std::vector<std::wstring> sheets;
        for (const auto& file : files) {
            if (Extension(file) == L".cue" || Extension(file) == L".m3u") sheets.push_back(Lower(file.substr(0, file.find_last_of(L'.'))));
        }
        for (const auto& file : files) {
            const std::wstring extension = Extension(file);
            if ((extension == L".bin" || extension == L".img") &&
                std::find(sheets.begin(), sheets.end(), Lower(file.substr(0, file.find_last_of(L'.')))) != sheets.end())
                continue;
            if (std::find(system->extensions.begin(), system->extensions.end(), extension) == system->extensions.end()) continue;
            if (id == L"SWITCH") {
                const SwitchContent content = DetectSwitchContent(file);
                if (content.kind == SwitchKind::Update || content.kind == SwitchKind::Addon) {
                    extras.push_back({file, content});
                    continue;
                }
            }
            const std::wstring key = FileKey(id, file);
            if (known(key)) continue;
            // The file's own contents still give the title and the serial where they can be read.
            const Detected found = DetectFile(file);
            std::wstring title = found.title;
            if (id == L"SWITCH") {
                // Dumps are named "Game [title id][version]": the name alone is the title.
                for (size_t open = title.find(L'['); open != std::wstring::npos; open = title.find(L'[')) {
                    const size_t close = title.find(L']', open);
                    if (close == std::wstring::npos) break;
                    title.erase(open, close - open + 1);
                }
                while (!title.empty() && title.back() == L' ') title.pop_back();
                if (title.empty()) title = found.title;
            }
            store(key, file, id, found.serial, title);
            arts.push_back({id, file, key});
            ++added;
        }
    }
    Load();
    ExtractArts(arts);
    for (const auto& game : games) {
        if (game.system != L"SWITCH" || extras.empty()) continue;
        const SwitchContent base = DetectSwitchContent(game.folder);
        if (base.kind != SwitchKind::Game) continue;
        std::string listed = ReadCoreOption(game, "eden_conteudo");
        const std::wstring have = L"|" + Lower(Wide(listed)) + L"|";
        bool changed = false;
        for (const auto& [file, content] : extras) {
            if (content.baseId != base.titleId || have.find(L"|" + Lower(file) + L"|") != std::wstring::npos) continue;
            listed += (listed.empty() ? "" : "|") + Utf8(file);
            changed = true;
        }
        if (changed) WriteCoreOption(game, "eden_conteudo", listed);
    }
    return added;
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
    const std::wstring fps = ReadIni(path, L"Bloodborne", L"fps", L"60");
    options.fps = fps == L"30" || fps == L"90" || fps == L"uncap" ? fps : L"60";
    const std::wstring render = ReadIni(path, L"Bloodborne", L"render_resolution", L"");
    options.renderResolution = render == L"1280x720" || render == L"2560x1440" || render == L"3840x2160" ? render : L"";
    options.dlss5 = ReadFlag(path, L"Enhancements", L"dlss5");
    return options;
}

namespace {

// Each line of core-options.ini as its key (empty for a line that is not "key = value") and the line itself.
std::vector<std::pair<std::string, std::string>> CoreOptionLines(const Game& game) {
    std::vector<std::pair<std::string, std::string>> lines;
    const std::string text = ReadBytes(game.workspace + L"\\core-options.ini");
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t equals = line.find('=');
        std::string key = equals == std::string::npos || line[0] == '#' || line[0] == ';' ? "" : line.substr(0, equals);
        while (!key.empty() && key.back() == ' ') key.pop_back();
        lines.push_back({key, line});
    }
    return lines;
}

}

std::string ReadCoreOption(const Game& game, const std::string& key) {
    for (const auto& [name, line] : CoreOptionLines(game)) {
        if (name != key) continue;
        std::string value = line.substr(line.find('=') + 1);
        while (!value.empty() && value.front() == ' ') value.erase(0, 1);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        return value;
    }
    return "";
}

void WriteCoreOption(const Game& game, const std::string& key, const std::string& value) {
    std::string out;
    bool replaced = false;
    for (const auto& [name, line] : CoreOptionLines(game)) {
        if (name == key) {
            if (replaced) continue;
            replaced = true;
            out += key + " = \"" + value + "\"\r\n";
        } else {
            out += line + "\r\n";
        }
    }
    if (!replaced) out += key + " = \"" + value + "\"\r\n";
    MakeFolders(game.workspace);
    FILE* file = nullptr;
    if (_wfopen_s(&file, (game.workspace + L"\\core-options.ini").c_str(), L"wb") != 0 || file == nullptr) return;
    std::fwrite(out.data(), 1, out.size(), file);
    std::fclose(file);
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
    MakeFolders(game.workspace);
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
    MakeFolders(game.workspace);
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
    WriteFlag(path, L"Enhancements", L"dlss5", options.dlss5);
    if (ProfileFor(game.titleId).bloodborne) {
        WritePrivateProfileStringW(L"Bloodborne", L"fps", options.fps.c_str(), path.c_str());
        WritePrivateProfileStringW(L"Bloodborne", L"render_resolution", options.renderResolution.c_str(), path.c_str());
    }
}

}
