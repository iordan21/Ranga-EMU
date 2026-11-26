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
    std::wstring source; // where the list came from (and where the files are fetched)
    std::string version, notes;
    std::string name; // the number people see ("1.0.0"); empty for a version published before numbers
    std::vector<UpdateFile> changed; // the files that differ from this PC's
    uint64_t download = 0;           // their size together
    std::wstring error;              // why there is nothing (empty: checked fine)
    bool declined = false;           // the newest is one the player went back from (RollBack)
    bool older = false;              // the newest there is older than this Ranga (it is on a beta)
};

// Where updates come from, tried in order and the newest signed list wins: engines.ini [Ranga] atualizacao (one or
// more, separated by ';'), the GitHub release (kGithubRepo's latest), and the address built in (Enderecos.hpp).
// Each is a "host:port" of a hosting PC's table server or an "https://..." folder of a release.
std::vector<std::wstring> UpdateSources(const std::wstring& root);
std::wstring UpdateSource(const std::wstring& root); // the first of them
// The version this PC last updated to or published (engines.ini [Ranga] versao_atualizacao).
std::string InstalledUpdate(const std::wstring& root);

// The PC that hosts: whether it can sign (has the key), and publishing the programs as they are now.
bool CanPublishUpdate(const std::wstring& root);
// wanted: the number to give it ("1.1.0"); empty: the last one with its last part one up (the first is 1.0.0).
// beta: only for who asked for betas (BetaChannel): its own folder (salas\atualizacao-beta, which the table server does
// not hand out), no entry in the news and no number spent.
std::wstring PublishUpdate(const std::wstring& root, const std::string& notes, std::string& version, const std::string& wanted = "", bool beta = false);
// This Ranga's number ("1.0.0"), empty when its version had none; true when text is such a number.
std::string InstalledVersionName(const std::wstring& root);
bool VersionName(const std::string& text);

// Every other PC: what is newer there, if anything (network; run off the window's thread).
UpdateInfo CheckUpdate(const std::wstring& root);
// Downloads the changed files to atualizacao-baixada and checks each one's hash; progress gets bytes done of total.
std::wstring DownloadUpdate(const std::wstring& root, const UpdateInfo& info, const std::function<void(uint64_t, uint64_t)>& progress);
// Puts the downloaded files in place (the running RangaEmu.exe is renamed to .old first, as Windows allows) and records
// the version. No game may be running.
std::wstring ApplyUpdate(const std::wstring& root, const UpdateInfo& info);
// ---- Going back ----
// ApplyUpdate keeps what it replaced in versao-anterior (one version back, only the files that changed). RollBack puts
// them in place again and notes the version left behind, which is not offered anymore (a newer one is).
struct PreviousVersion {
    std::string version, name; // empty version: there is none to go back to
    std::vector<UpdateFile> files;
};
PreviousVersion Previous(const std::wstring& root);
std::wstring RollBack(const std::wstring& root);
std::string DeclinedUpdate(const std::wstring& root);
void ForgetDeclined(const std::wstring& root);
// The first starts after an update: UpdateNeverOpened counts this start and is true when the one before never got to
// ConfirmUpdate (the new Ranga breaks as it opens).
bool UpdateNeverOpened(const std::wstring& root);
void ConfirmUpdate(const std::wstring& root);

// Betas (engines.ini [Ranga] beta=1): the beta release counts as a source too. A signed list that says it is a beta is
// refused everywhere else.
bool BetaChannel(const std::wstring& root);
void SetBetaChannel(const std::wstring& root, bool on);

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

// The hosting PC: the published version (salas\atualizacao) also as a release on GitHub, through the gh command
// (signed in on this PC), so friends get it with this PC turned off. The three newest releases stay. Uploads everything
// (about 450 MB): minutes, off the window's thread.
// beta: the published beta, as the one release tagged "beta" (a prerelease, the last one replaced).
std::wstring PublishToGitHub(const std::wstring& root, std::string& version, bool beta = false);

// The hosting PC: a whole Ranga for a friend who has none (a new or formatted PC), as one zip in folder: the programs,
// the cores and the emulators, with this PC's engines.ini minus what is only this PC's (server, group, backup,
// quality). Never BIOS, firmware, keys, games, saves, the profile, the library or the signing and owner keys.
// Uses Windows' own tar.exe; takes a minute or two (off the window's thread). made gets the zip's path.
std::wstring MakeFriendPackage(const std::wstring& root, const std::wstring& folder, std::wstring& made);

// ---- What the apps' store (Store.hpp) shares with the update ----

// GET of a whole http(s) address (redirects followed, as GitHub's downloads need): the status, 0 when unreachable;
// the body to memory or to a file.
int DownloadUrl(const std::wstring& url, std::string* body, const std::wstring& toFile, const std::function<void(uint64_t)>& got);
// Signed with the key only the hosting PC has (the same as the update's lists).
bool SignedByRanga(const std::string& message, const std::string& signature);
// The hosting PC: signs with salas\chave-atualizacao.bin. Empty: done; else why not.
std::wstring SignWithRangaKey(const std::wstring& root, const std::string& message, std::string& signature);
std::string Sha256OfFile(const std::wstring& path);
// Runs a command hidden and waits; out gets what it printed. The exit code, -1 when it did not start.
int RunHiddenCommand(const std::wstring& command, const std::wstring& folder, std::string& out);
void RemoveFolderTree(const std::wstring& folder);

}

#endif
