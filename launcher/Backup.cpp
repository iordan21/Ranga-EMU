#include "Backup.hpp"
#include "Common.hpp"

#include <windows.h>

#include <algorithm>
#include <ctime>
#include <cwchar>
#include <vector>

namespace Ranga {

namespace {

std::wstring Ini(const std::wstring& root) { return root + L"\\engines.ini"; }

struct Item {
    std::wstring from, to; // the file, and where it goes inside the copy (relative)
    long long written = 0; // last write, Unix time
};

long long UnixTime(const FILETIME& time) {
    ULARGE_INTEGER value;
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return static_cast<long long>(value.QuadPart / 10000000ULL) - 11644473600LL;
}

// Every file under folder (relative names under base), leaving out the folders whose lower-case name is in skip.
void Collect(const std::wstring& folder, const std::wstring& base, const std::vector<std::wstring>& skip, std::vector<Item>& items) {
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((folder + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = found.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring full = folder + L"\\" + name, rel = base + L"\\" + name;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (std::find(skip.begin(), skip.end(), Lower(name)) == skip.end()) Collect(full, rel, skip, items);
        } else {
            items.push_back({full, rel, UnixTime(found.ftLastWriteTime)});
        }
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

// The save folders inside the cores' and emulators' folders: any folder named like one, down to depth levels (the
// saved states' folders too, unless savesOnly).
void SaveFolders(const std::wstring& folder, const std::wstring& base, int depth, std::vector<Item>& items, bool savesOnly = false) {
    static const std::vector<std::wstring> kAll = {L"save", L"saves", L"memcards", L"sstates", L"states", L"gamesaves", L"savedata"};
    static const std::vector<std::wstring> kSaves = {L"save", L"saves", L"memcards", L"gamesaves", L"savedata"};
    const std::vector<std::wstring>& kNames = savesOnly ? kSaves : kAll;
    // Firmware, keys, shaders and other big things that are never saves.
    static const std::vector<std::wstring> kNever = {L"contents", L"shader", L"shadercache", L"cache", L"keys", L"registered", L"assets",
                                                     L"database", L"cores", L"_downloads", L"dev_flash"};
    if (depth < 0) return;
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((folder + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = found.cFileName, lower = Lower(name);
        if (name == L"." || name == L".." || (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            continue;
        const std::wstring full = folder + L"\\" + name, rel = base + L"\\" + name;
        if (std::find(kNames.begin(), kNames.end(), lower) != kNames.end()) Collect(full, rel, savesOnly ? std::vector<std::wstring>{L"sstates", L"states"} : std::vector<std::wstring>{}, items);
        else if (std::find(kNever.begin(), kNever.end(), lower) == kNever.end() && (!savesOnly || (lower != L"sstates" && lower != L"states")))
            SaveFolders(full, rel, depth - 1, items, savesOnly);
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

std::vector<Item> WhatToCopy(const std::wstring& root) {
    std::vector<Item> items;
    Collect(root + L"\\saves", L"saves", {L"logs", L"art"}, items);
    Collect(root + L"\\perfil", L"perfil", {L"amigos"}, items);
    SaveFolders(root + L"\\bios", L"bios", 6, items);
    SaveFolders(root + L"\\emuladores", L"emuladores", 7, items);
    return items;
}

void MakeParents(const std::wstring& path) {
    for (size_t at = path.find(L'\\', 3); at != std::wstring::npos; at = path.find(L'\\', at + 1)) CreateDirectoryW(path.substr(0, at).c_str(), nullptr);
}

void RemoveTree(const std::wstring& folder) {
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((folder + L"\\*").c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = found.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring full = folder + L"\\" + name;
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) RemoveTree(full);
            else {
                SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(full.c_str());
            }
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    RemoveDirectoryW(folder.c_str());
}

}

std::vector<std::pair<std::wstring, std::wstring>> CloudSaveFiles(const std::wstring& root) {
    std::vector<Item> items;
    // saves\<console>\<game>\saves: the cores' save files (.srm and the like), not the options or logs beside them.
    WIN32_FIND_DATAW console;
    HANDLE consoles = FindFirstFileW((root + L"\\saves\\*").c_str(), &console);
    if (consoles != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring c = console.cFileName;
            if (c == L"." || c == L".." || (console.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
            WIN32_FIND_DATAW game;
            HANDLE games = FindFirstFileW((root + L"\\saves\\" + c + L"\\*").c_str(), &game);
            if (games == INVALID_HANDLE_VALUE) continue;
            do {
                const std::wstring g = game.cFileName;
                if (g == L"." || g == L".." || (game.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
                Collect(root + L"\\saves\\" + c + L"\\" + g + L"\\saves", L"saves\\" + c + L"\\" + g + L"\\saves", {}, items);
            } while (FindNextFileW(games, &game));
            FindClose(games);
        } while (FindNextFileW(consoles, &console));
        FindClose(consoles);
    }
    SaveFolders(root + L"\\bios", L"bios", 6, items, true);
    SaveFolders(root + L"\\emuladores", L"emuladores", 7, items, true);
    std::vector<std::pair<std::wstring, std::wstring>> out;
    for (const Item& item : items) {
        const std::wstring lower = Lower(item.to);
        // Saved states (F5 in Ranga Retro) stay on the PC; so do the Switch's system saves, which belong to the console.
        if (lower.find(L".state") != std::wstring::npos || lower.find(L"\\nand\\system\\") != std::wstring::npos) continue;
        out.emplace_back(item.from, item.to);
    }
    return out;
}

std::wstring BackupFolder(const std::wstring& root) { return ReadIni(Ini(root), L"Ranga", L"backup", L""); }

void SetBackupFolder(const std::wstring& root, const std::wstring& folder) {
    EnsureUnicodeIni(Ini(root));
    WritePrivateProfileStringW(L"Ranga", L"backup", folder.empty() ? nullptr : folder.c_str(), Ini(root).c_str());
}

long long LastBackup(const std::wstring& root) { return std::wcstoll(ReadIni(Ini(root), L"Ranga", L"backup_quando", L"0").c_str(), nullptr, 10); }

std::wstring BackupNow(const std::wstring& root, bool force, std::wstring& made, int& files) {
    made.clear();
    files = 0;
    const std::wstring target = BackupFolder(root);
    if (target.empty()) return L"Escolha primeiro a pasta do backup.";
    if (!IsDirectory(target)) return L"A pasta do backup não existe mais: " + target;
    // A copy inside Ranga's own folder would copy itself the next time.
    if (Lower(target + L"\\").rfind(Lower(root + L"\\"), 0) == 0) return L"A pasta do backup não pode ficar dentro da pasta do Ranga.";
    const std::vector<Item> items = WhatToCopy(root);
    if (items.empty()) return L"Ainda não há nenhum save para copiar.";
    long long newest = 0;
    for (const Item& item : items) newest = std::max(newest, item.written);
    const long long last = LastBackup(root);
    if (!force && last != 0 && newest <= last) return L"";

    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    wchar_t stamp[32];
    wcsftime(stamp, 32, L"%Y-%m-%d_%H%M%S", &local);
    const std::wstring base = target + L"\\Ranga-backup", folder = base + L"\\" + stamp, partial = folder + L".parcial";
    CreateDirectoryW(base.c_str(), nullptr);
    RemoveTree(partial);
    for (const Item& item : items) {
        const std::wstring to = partial + L"\\" + item.to;
        MakeParents(to);
        if (!CopyFileW(item.from.c_str(), to.c_str(), FALSE)) {
            RemoveTree(partial);
            return L"Não deu para copiar " + item.to + L" (disco cheio ou arquivo em uso?).";
        }
        ++files;
    }
    // Complete copies only get their real name: a half copy (PC turned off midway) never looks like a good one.
    if (!MoveFileW(partial.c_str(), folder.c_str())) {
        RemoveTree(partial);
        return L"Não deu para fechar a cópia em " + folder + L".";
    }
    made = folder;
    EnsureUnicodeIni(Ini(root));
    WritePrivateProfileStringW(L"Ranga", L"backup_quando", std::to_wstring(std::max<long long>(newest, now)).c_str(), Ini(root).c_str());

    // The oldest copies go once there are more than kBackupsKept (the names sort by date).
    std::vector<std::wstring> copies;
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((base + L"\\*").c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = found.cFileName;
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && name.size() == 17 && name[4] == L'-' && name[10] == L'_') copies.push_back(name);
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    std::sort(copies.begin(), copies.end());
    for (size_t i = 0; i + kBackupsKept < copies.size(); ++i) RemoveTree(base + L"\\" + copies[i]);
    return L"";
}

}
