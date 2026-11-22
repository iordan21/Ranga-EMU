#ifndef LAUNCHER_CLOUD_HPP
#define LAUNCHER_CLOUD_HPP

// The saves in the cloud: the account's saves (Backup.hpp CloudSaveFiles) on the central (launcher/net/cloudflare,
// /nuvem), so a game goes on from another PC. Each file is sent packed (zlib) in pieces, and only when it changed
// since this PC last met the cloud (perfil\nuvem-<account>.txt keeps the version of each file it last sent or got).
// Nothing is lost on its own: a save changed here and on another PC since then is a conflict, left for the player to
// choose; a file brought from the cloud first goes, as it was here, to backups\nuvem\<date>.

#include <atomic>
#include <string>
#include <vector>

namespace Ranga {

// engines.ini [Ranga] nuvem=1: off until the player turns it on.
bool CloudEnabled(const std::wstring& root);
void SetCloudEnabled(const std::wstring& root, bool on);

struct CloudReport {
    int sent = 0, got = 0, same = 0;
    // Newer in the cloud than here (from another PC), and changed both here and there.
    std::vector<std::wstring> newer, conflicts;
    std::string newerFrom; // the PC the newest of them came from
    long long used = 0, quota = 0;
    std::string error; // empty: all went well
    bool done = false;
};

// Sends what changed here. force: a conflict goes as this PC has it (the cloud's version is replaced).
CloudReport CloudUpload(const std::wstring& root, const std::string& account, const std::string& session, bool force, std::atomic<int>* progress = nullptr);
// What the cloud has that this PC does not (newer and conflicts), without changing anything.
CloudReport CloudCheck(const std::wstring& root, const std::string& account, const std::string& session);
// Brings the cloud's newer saves here; with conflictsToo, the conflicts too (the cloud's version wins).
CloudReport CloudDownload(const std::wstring& root, const std::string& account, const std::string& session, bool conflictsToo, std::atomic<int>* progress = nullptr);
// Removes everything of this account from the cloud (the saves here stay).
std::string CloudClear(const std::wstring& root, const std::string& account, const std::string& session);

}

#endif
