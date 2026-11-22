#include "Cloud.hpp"
#include "Backup.hpp"
#include "Common.hpp"
#include "Media.hpp"
#include "PlayerProfile.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <map>

namespace Ranga {

namespace {

std::wstring Ini(const std::wstring& root) { return root + L"\\engines.ini"; }

// The base64 of a piece: under the central's 700 KB, and a multiple of 4 so each piece decodes alone.
constexpr size_t kPiece = 699 * 1024;
// A file bigger than this (unpacked) is no save; it stays out.
constexpr size_t kBiggest = 96u * 1024 * 1024;

struct Remote {
    std::string hash, pc;
    long long size = 0, raw = 0, parts = 0, when = 0;
};

std::vector<std::string> Split(const std::string& line, char by) {
    std::vector<std::string> parts;
    for (size_t from = 0;;) {
        const size_t at = line.find(by, from);
        parts.push_back(line.substr(from, at == std::string::npos ? std::string::npos : at - from));
        if (at == std::string::npos) break;
        from = at + 1;
    }
    return parts;
}

std::wstring StateFile(const std::wstring& root, const std::string& account) { return root + L"\\perfil\\nuvem-" + Wide(account) + L".txt"; }

std::map<std::string, std::string> ReadState(const std::wstring& root, const std::string& account) {
    std::map<std::string, std::string> state;
    for (const auto& line : Split(ReadBytes(StateFile(root, account)), '\n')) {
        const size_t tab = line.find('\t');
        if (tab != std::string::npos) state[line.substr(0, tab)] = line.substr(tab + 1, 16);
    }
    return state;
}

void WriteState(const std::wstring& root, const std::string& account, const std::map<std::string, std::string>& state) {
    std::string text;
    for (const auto& [path, hash] : state) text += path + "\t" + hash + "\n";
    const std::wstring file = StateFile(root, account), temp = file + L".novo";
    HANDLE handle = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    const bool ok = WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &put, nullptr) && put == text.size();
    CloseHandle(handle);
    if (ok) MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// The Switch's saves sit under the emulator's user, a random id each PC has its own of: in the cloud that part is
// "@usuario", and here it is the one user this PC's Eden has.
constexpr const wchar_t* kEdenSaves = L"emuladores\\eden\\user\\nand\\user\\save\\0000000000000000\\";

bool HexName(const std::wstring& name) {
    if (name.size() != 32) return false;
    for (const wchar_t c : name) if (!iswxdigit(c)) return false;
    return true;
}

std::wstring CloudName(const std::wstring& rel) {
    const std::wstring lower = Lower(rel), prefix = Lower(std::wstring(kEdenSaves));
    if (lower.rfind(prefix, 0) != 0) return rel;
    const size_t start = prefix.size(), end = rel.find(L'\\', start);
    if (end == std::wstring::npos || !HexName(rel.substr(start, end - start))) return rel;
    return rel.substr(0, start) + L"@usuario" + rel.substr(end);
}

// The path here of a cloud name; empty when it cannot be (no Eden user here yet, or a name that leaves the folder).
std::wstring LocalName(const std::wstring& root, const std::wstring& name) {
    if (name.empty() || name.find(L"..") != std::wstring::npos || name.find(L'/') != std::wstring::npos || name.find(L':') != std::wstring::npos) return L"";
    const std::wstring lower = Lower(name);
    if (lower.rfind(L"saves\\", 0) != 0 && lower.rfind(L"bios\\", 0) != 0 && lower.rfind(L"emuladores\\", 0) != 0) return L"";
    const std::wstring marker = std::wstring(kEdenSaves) + L"@usuario\\";
    if (lower.rfind(Lower(marker), 0) != 0) return name;
    std::vector<std::wstring> users;
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((root + L"\\" + kEdenSaves + L"*").c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && HexName(found.cFileName)) users.push_back(found.cFileName);
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    if (users.size() != 1) return L"";
    return std::wstring(kEdenSaves) + users[0] + name.substr(marker.size() - 1);
}

void MakeParents(const std::wstring& path) {
    for (size_t at = path.find(L'\\', 3); at != std::wstring::npos; at = path.find(L'\\', at + 1)) CreateDirectoryW(path.substr(0, at).c_str(), nullptr);
}

bool WriteWhole(const std::wstring& path, const std::string& bytes) {
    MakeParents(path);
    const std::wstring temp = path + L".nuvem";
    HANDLE handle = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    const bool ok = WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &put, nullptr) && put == bytes.size();
    CloseHandle(handle);
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

std::string PcName() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    return GetComputerNameW(name, &size) ? Utf8(name) : "PC";
}

std::string Problem(const HttpResult& got) {
    if (got.status == 0) return "O servidor do Ranga não respondeu. Confira a internet.";
    if (got.status == 401) return "A sessão acabou: entre na sua conta de novo.";
    if (got.status == 404) return "O servidor do Ranga ainda não tem o save na nuvem.";
    const std::string why = ReplyField(got.body, "erro");
    return why.empty() ? "O servidor do Ranga respondeu com erro " + std::to_string(got.status) + "." : why;
}

bool List(const std::wstring& root, const std::string& session, std::map<std::string, Remote>& out, CloudReport& report) {
    const HttpResult got = CentralCall(root, "GET", "/nuvem", "", session, 20000);
    if (got.status != 200) {
        report.error = Problem(got);
        return false;
    }
    for (const auto& line : Split(got.body, '\n')) {
        const auto parts = Split(line, '\t');
        if (parts.size() == 8 && parts[0] == "a") {
            out[parts[1]] = {parts[2], parts[7], std::atoll(parts[3].c_str()), std::atoll(parts[4].c_str()), std::atoll(parts[5].c_str()), std::atoll(parts[6].c_str())};
        } else if (parts.size() == 3 && parts[0] == "cota") {
            report.used = std::atoll(parts[1].c_str());
            report.quota = std::atoll(parts[2].c_str());
        }
    }
    return true;
}

// Every save here by its cloud name: the file and its version now.
struct Local {
    std::wstring path;
    std::string hash;
    std::string bytes;
};

std::map<std::string, Local> Scan(const std::wstring& root, bool keepBytes) {
    std::map<std::string, Local> out;
    for (const auto& [full, rel] : CloudSaveFiles(root)) {
        std::string bytes = ReadBytes(full);
        if (bytes.size() > kBiggest) continue;
        Local local{full, LocalHash(bytes), {}};
        if (keepBytes) local.bytes = std::move(bytes);
        out[Utf8(CloudName(rel))] = std::move(local);
    }
    return out;
}

std::string Stamp() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d_%H%M%S", &local);
    return text;
}

}

bool CloudEnabled(const std::wstring& root) { return ReadIni(Ini(root), L"Ranga", L"nuvem", L"0") == L"1"; }

void SetCloudEnabled(const std::wstring& root, bool on) {
    EnsureUnicodeIni(Ini(root));
    WritePrivateProfileStringW(L"Ranga", L"nuvem", on ? L"1" : L"0", Ini(root).c_str());
    ForgetIni(Ini(root));
}

CloudReport CloudUpload(const std::wstring& root, const std::string& account, const std::string& session, bool force, std::atomic<int>* progress) {
    CloudReport report;
    std::map<std::string, Remote> remote;
    if (!List(root, session, remote, report)) return report;
    auto state = ReadState(root, account);
    const std::string pc = PcName();
    for (auto& [name, local] : Scan(root, true)) {
        const auto there = remote.find(name);
        const std::string synced = state.count(name) ? state[name] : "";
        if (there != remote.end() && there->second.hash == local.hash) {
            state[name] = local.hash;
            ++report.same;
            continue;
        }
        if (there != remote.end()) {
            // Not changed here since the last meeting: the cloud's is newer (from another PC); download brings it.
            if (local.hash == synced) {
                report.newer.push_back(Wide(name));
                continue;
            }
            // Changed here and there: the player chooses.
            if (there->second.hash != synced && !force) {
                report.conflicts.push_back(Wide(name));
                continue;
            }
        }
        const std::string deflated = Deflate(local.bytes);
        if (deflated.empty() && !local.bytes.empty()) continue;
        const std::string packed = Base64(deflated);
        const size_t pieces = std::max<size_t>(1, (packed.size() + kPiece - 1) / kPiece);
        bool ok = true;
        for (size_t i = 0; i < pieces && ok; ++i) {
            const std::string piece = packed.substr(i * kPiece, kPiece);
            const HttpResult got = CentralCall(root, "POST", "/nuvem/parte", "hash\t" + local.hash + "\nindice\t" + std::to_string(i) + "\ndados\t" + piece + "\n", session, 60000);
            if (got.status != 200) {
                report.error = Problem(got);
                ok = false;
            }
        }
        if (!ok) break;
        const std::string before = there != remote.end() ? there->second.hash : "";
        const HttpResult closed = CentralCall(root, "POST", "/nuvem/fechar",
                                              "caminho\t" + name + "\nhash\t" + local.hash + "\ntamanho\t" + std::to_string(deflated.size()) + "\nbruto\t" +
                                                  std::to_string(local.bytes.size()) + "\npartes\t" + std::to_string(pieces) + "\nantes\t" + before + "\npc\t" + pc + "\n",
                                              session, 30000);
        if (closed.status == 409) {
            report.conflicts.push_back(Wide(name));
            continue;
        }
        if (closed.status != 200) {
            report.error = Problem(closed);
            if (closed.status == 400) continue; // this file only (too big)
            break;
        }
        state[name] = local.hash;
        ++report.sent;
        if (progress != nullptr) ++*progress;
    }
    WriteState(root, account, state);
    if (report.error.empty()) {
        // The space used, as the central counts it now.
        std::map<std::string, Remote> after;
        CloudReport count;
        if (List(root, session, after, count)) report.used = count.used, report.quota = count.quota;
    }
    report.done = true;
    return report;
}

CloudReport CloudCheck(const std::wstring& root, const std::string& account, const std::string& session) {
    CloudReport report;
    std::map<std::string, Remote> remote;
    if (!List(root, session, remote, report)) return report;
    auto state = ReadState(root, account);
    const auto here = Scan(root, false);
    long long newest = 0;
    for (const auto& [name, file] : remote) {
        const auto local = here.find(name);
        if (local != here.end() && local->second.hash == file.hash) continue;
        if (state.count(name) && state[name] == file.hash) continue; // sent from here; changed here since (upload sends it)
        const bool changedHere = local != here.end() && (!state.count(name) || state[name] != local->second.hash);
        (changedHere ? report.conflicts : report.newer).push_back(Wide(name));
        if (file.when > newest) newest = file.when, report.newerFrom = file.pc;
    }
    report.done = true;
    return report;
}

CloudReport CloudDownload(const std::wstring& root, const std::string& account, const std::string& session, bool conflictsToo, std::atomic<int>* progress) {
    CloudReport report;
    std::map<std::string, Remote> remote;
    if (!List(root, session, remote, report)) return report;
    auto state = ReadState(root, account);
    const auto here = Scan(root, false);
    const std::wstring keep = root + L"\\backups\\nuvem\\" + Wide(Stamp());
    for (const auto& [name, file] : remote) {
        const auto local = here.find(name);
        if (local != here.end() && local->second.hash == file.hash) {
            state[name] = file.hash;
            ++report.same;
            continue;
        }
        if (state.count(name) && state[name] == file.hash) continue;
        const bool changedHere = local != here.end() && (!state.count(name) || state[name] != local->second.hash);
        if (changedHere && !conflictsToo) {
            report.conflicts.push_back(Wide(name));
            continue;
        }
        const std::wstring rel = LocalName(root, Wide(name));
        if (rel.empty()) {
            report.error = "Alguns saves do Switch ficaram na nuvem: abra um jogo de Switch neste PC uma vez e baixe de novo.";
            continue;
        }
        std::string packed;
        bool ok = true;
        for (long long i = 0; i < file.parts && ok; ++i) {
            const HttpResult got = CentralCall(root, "GET", "/nuvem/parte?hash=" + file.hash + "&indice=" + std::to_string(i), "", session, 60000);
            if (got.status != 200) {
                report.error = Problem(got);
                ok = false;
            } else {
                packed += got.body;
            }
        }
        if (!ok) break;
        std::string bytes;
        if (!Inflate(Unbase64(packed), static_cast<size_t>(file.raw), bytes) || LocalHash(bytes) != file.hash) {
            report.error = "Um save veio estragado da nuvem e não foi usado: " + name;
            continue;
        }
        const std::wstring target = root + L"\\" + rel;
        // What was here goes to backups\nuvem first.
        if (Exists(target)) {
            const std::wstring copy = keep + L"\\" + rel;
            MakeParents(copy);
            CopyFileW(target.c_str(), copy.c_str(), FALSE);
        }
        if (!WriteWhole(target, bytes)) {
            report.error = "Não deu para gravar " + Utf8(rel) + " (o jogo está aberto?).";
            continue;
        }
        state[name] = file.hash;
        ++report.got;
        if (progress != nullptr) ++*progress;
    }
    WriteState(root, account, state);
    report.done = true;
    return report;
}

std::string CloudClear(const std::wstring& root, const std::string& account, const std::string& session) {
    const HttpResult got = CentralCall(root, "POST", "/nuvem/limpar", "", session, 20000);
    if (got.status != 200) return Problem(got);
    DeleteFileW(StateFile(root, account).c_str());
    return "";
}

}
