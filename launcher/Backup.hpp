#ifndef LAUNCHER_BACKUP_HPP
#define LAUNCHER_BACKUP_HPP

// Copies of the player's saves in a folder they choose (engines.ini [Ranga] backup), such as the Google Drive folder on
// the PC: one dated folder per copy in <folder>\Ranga-backup, the last kBackupsKept kept. What goes: saves\ (each
// game's folder, without logs and art), perfil\ (without the friends' media) and every save, memory card and saved
// state folder of the cores (bios\) and the emulators (emuladores\). Never games, BIOS, firmware or keys.

#include <string>
#include <utility>
#include <vector>

namespace Ranga {

constexpr int kBackupsKept = 10;

std::wstring BackupFolder(const std::wstring& root); // empty: no backup
void SetBackupFolder(const std::wstring& root, const std::wstring& folder);
// When the last copy was made (engines.ini [Ranga] backup_quando, Unix time), 0 for never.
long long LastBackup(const std::wstring& root);

// Makes a copy now; skipped (with made empty) when nothing changed since the last one, unless force. Error text, or
// empty; made gets the folder of the copy. Reads and writes files: off the window's thread.
std::wstring BackupNow(const std::wstring& root, bool force, std::wstring& made, int& files);

// What the save in the cloud takes (Cloud.cpp): only real saves, as (the file, its path from root). The saves
// folders of each game (without saved states), and the save, memory card and save data folders of the cores and
// emulators (without saved states and the Switch's system saves). Never perfil\ (the session key is there).
std::vector<std::pair<std::wstring, std::wstring>> CloudSaveFiles(const std::wstring& root);

}

#endif
