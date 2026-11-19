#ifndef LAUNCHER_UPDATE_HPP
#define LAUNCHER_UPDATE_HPP

// Ranga updating itself from the PC that hosts. That PC publishes a version (PublishUpdate): Ranga's programs and the
// cores, copied to salas\atualizacao with a list of their SHA-256 (manifest.txt) signed with the key only that PC has
// (salas\chave-atualizacao.bin, ECDSA P-256). The table server (tools\ranga-sala.exe) hands them out over HTTP on its
// own port, so the friends' Ranga reaches them through the same playit tunnel as online play. Every other Ranga
// checks when it opens (CheckUpdate): only a list signed with that key counts, and only files whose hash matches it
// are put in place. Keys, firmware, BIOS and games are never part of it.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Ranga {

struct UpdateFile {
    std::wstring path; // relative to Ranga's folder, with backslashes
    std::string sha256;
    uint64_t size = 0;
};

struct UpdateInfo {
    std::string version, notes;
    std::vector<UpdateFile> changed; // the files that differ from this PC's
    uint64_t download = 0;           // their size together
    std::wstring error;              // why there is nothing (empty: checked fine)
};

// The address updates come from ("host:port"): engines.ini [Ranga] atualizacao, else the classics' playit tunnel.
std::wstring UpdateSource(const std::wstring& root);
// The version this PC last updated to or published (engines.ini [Ranga] versao_atualizacao).
std::string InstalledUpdate(const std::wstring& root);

// The PC that hosts: whether it can sign (has the key), and publishing the programs as they are now.
bool CanPublishUpdate(const std::wstring& root);
std::wstring PublishUpdate(const std::wstring& root, const std::string& notes, std::string& version);

// Every other PC: what is newer there, if anything (network; run off the window's thread).
UpdateInfo CheckUpdate(const std::wstring& root);
// Downloads the changed files to atualizacao-baixada and checks each one's hash; progress gets bytes done of total.
std::wstring DownloadUpdate(const std::wstring& root, const UpdateInfo& info, const std::function<void(uint64_t, uint64_t)>& progress);
// Puts the downloaded files in place (the running RangaEmu.exe is renamed to .old first, as Windows allows) and records
// the version. No game may be running.
std::wstring ApplyUpdate(const std::wstring& root, const UpdateInfo& info);
// Removes what an update left behind (*.old, the download folder) once the new Ranga runs.
void CleanUpdateLeftovers(const std::wstring& root);

// Checking Ranga's files (the programs and the cores, what an update carries) against the signed list of the latest
// version: which are missing and which differ (broken, or older than that version). Elsewhere the list comes from the
// hosting PC and what is wrong can be fetched again (repair, through DownloadUpdate and ApplyUpdate). On the hosting PC
// it is its own published copy: a difference there is usually a newer build not published yet, so nothing is repaired.
// Emulators, BIOS, saves and games are not part of an update and are not checked. Reads every file: off the window's
// thread; progress gets files done of total.
struct IntegrityReport {
    std::string version;   // the version the files were checked against
    bool host = false;     // this PC publishes the updates
    bool behind = false;   // this PC was on an older version (some differences are just the update)
    int checked = 0;
    std::vector<std::wstring> missing, different;
    UpdateInfo repair;     // the missing and different files, to download again
    std::wstring error;    // why nothing was checked
};
IntegrityReport CheckIntegrity(const std::wstring& root, const std::function<void(int, int)>& progress);

// The hosting PC: a whole Ranga for a friend who has none (a new or formatted PC), as one zip in folder: the programs,
// the cores and the emulators, with this PC's engines.ini minus what is only this PC's (server, group, backup,
// quality). Never BIOS, firmware, keys, games, saves, the profile, the library or the signing and owner keys.
// Uses Windows' own tar.exe; takes a minute or two (off the window's thread). made gets the zip's path.
std::wstring MakeFriendPackage(const std::wstring& root, const std::wstring& folder, std::wstring& made);

}

#endif
