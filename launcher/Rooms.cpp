#include "Rooms.hpp"
#include "Common.hpp"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>

namespace Ranga {

namespace {

std::wstring Ini(const std::wstring& root) { return root + L"\\salas.ini"; }
std::wstring Folder(const std::wstring& root) { return root + L"\\salas"; }
std::wstring Server(const std::wstring& root) { return root + L"\\tools\\eden-room.exe"; }

int ReadNumber(const std::wstring& ini, const std::wstring& section, const wchar_t* key, int fallback) {
    const std::wstring text = ReadIni(ini, section.c_str(), key, L"");
    return text.empty() ? fallback : _wtoi(text.c_str());
}

// The process's own file, or empty when it is gone or not to be read.
std::wstring ProcessPath(unsigned long pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) return L"";
    wchar_t path[MAX_PATH * 2] = {};
    DWORD size = MAX_PATH * 2;
    std::wstring result;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) result = path;
    DWORD code = 0;
    if (GetExitCodeProcess(process, &code) && code != STILL_ACTIVE) result.clear();
    CloseHandle(process);
    return result;
}

std::wstring StartupShortcut() {
    wchar_t* startup = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &startup))) path = std::wstring(startup) + L"\\Ranga Emu - salas.lnk";
    CoTaskMemFree(startup);
    return path;
}

}

bool RoomServerEnabled(const std::wstring& root) { return ReadIni(root + L"\\engines.ini", L"Ranga", L"servidor", L"0") == L"1"; }

void SetRoomServerEnabled(const std::wstring& root, bool on) {
    const std::wstring ini = root + L"\\engines.ini";
    EnsureUnicodeIni(ini);
    WritePrivateProfileStringW(L"Ranga", L"servidor", on ? L"1" : nullptr, ini.c_str());
}

std::vector<Room> LoadRooms(const std::wstring& root) {
    std::vector<Room> rooms;
    const std::wstring ini = Ini(root);
    std::vector<wchar_t> names(32768);
    const DWORD length = GetPrivateProfileSectionNamesW(names.data(), static_cast<DWORD>(names.size()), ini.c_str());
    for (const wchar_t* name = names.data(); name < names.data() + length && *name != L'\0'; name += wcslen(name) + 1) {
        if (_wcsnicmp(name, L"Sala", 4) != 0) continue;
        Room room;
        room.id = name;
        room.name = ReadIni(ini, name, L"nome", L"Sala do Ranga");
        room.password = ReadIni(ini, name, L"senha", L"");
        room.port = std::clamp(ReadNumber(ini, name, L"porta", 24872), 1, 65535);
        room.members = std::clamp(ReadNumber(ini, name, L"maximo", 16), 2, 16);
        room.on = ReadNumber(ini, name, L"ligada", 0) != 0;
        rooms.push_back(room);
    }
    std::sort(rooms.begin(), rooms.end(), [](const Room& a, const Room& b) { return a.port < b.port; });
    return rooms;
}

void SaveRoom(const std::wstring& root, const Room& room) {
    const std::wstring ini = Ini(root);
    EnsureUnicodeIni(ini);
    WritePrivateProfileStringW(room.id.c_str(), L"nome", room.name.c_str(), ini.c_str());
    WritePrivateProfileStringW(room.id.c_str(), L"senha", room.password.empty() ? nullptr : room.password.c_str(), ini.c_str());
    WritePrivateProfileStringW(room.id.c_str(), L"porta", std::to_wstring(room.port).c_str(), ini.c_str());
    WritePrivateProfileStringW(room.id.c_str(), L"maximo", std::to_wstring(room.members).c_str(), ini.c_str());
    WritePrivateProfileStringW(room.id.c_str(), L"ligada", room.on ? L"1" : L"0", ini.c_str());
}

void RemoveRoom(const std::wstring& root, const std::wstring& id) {
    for (const Room& room : LoadRooms(root)) {
        if (room.id == id) StopRoomServer(root, room);
    }
    WritePrivateProfileStringW(id.c_str(), nullptr, nullptr, Ini(root).c_str());
}

Room AddRoom(const std::wstring& root) {
    const auto rooms = LoadRooms(root);
    Room room;
    int number = 1;
    for (bool taken = true; taken; ++number) {
        taken = false;
        for (const Room& other : rooms) taken = taken || _wcsicmp(other.id.c_str(), (L"Sala" + std::to_wstring(number)).c_str()) == 0;
        if (!taken) room.id = L"Sala" + std::to_wstring(number);
    }
    room.port = 24872;
    for (bool taken = true; taken;) {
        taken = false;
        for (const Room& other : rooms) taken = taken || other.port == room.port;
        if (taken) ++room.port;
    }
    room.name = rooms.empty() ? L"Sala do Ranga" : L"Sala " + std::to_wstring(rooms.size() + 1);
    SaveRoom(root, room);
    return room;
}

bool RoomRunning(const std::wstring& root, const Room& room) {
    const unsigned long pid = static_cast<unsigned long>(ReadNumber(Ini(root), room.id, L"pid", 0));
    if (pid == 0) return false;
    const std::wstring path = ProcessPath(pid);
    // A pid from an old start may now be any other program's: only Ranga's own server counts.
    return !path.empty() && _wcsicmp(path.c_str(), Server(root).c_str()) == 0;
}

std::wstring StartRoomServer(const std::wstring& root, const Room& room) {
    if (RoomRunning(root, room)) return L"";
    if (!Exists(Server(root))) return L"Falta o servidor de sala em " + Server(root) + L".";
    CreateDirectoryW(Folder(root).c_str(), nullptr);
    // Eden's room reads its ban list only when the file starts with its own header.
    const std::wstring bans = Folder(root) + L"\\" + room.id + L"-banidos.txt";
    if (!Exists(bans)) {
        if (FILE* file = _wfopen(bans.c_str(), L"wb")) {
            std::fputs("YuzuRoom-BanList-1\n\n", file);
            std::fclose(file);
        }
    }
    const auto quote = [](const std::wstring& text) {
        std::wstring out = L"\"";
        for (const wchar_t c : text) out += c == L'"' ? std::wstring(L"'") : std::wstring(1, c);
        return out + L"\"";
    };
    std::wstring command = quote(Server(root)) + L" --room-name " + quote(room.name.empty() ? L"Sala do Ranga" : room.name) + L" --port " +
                           std::to_wstring(room.port) + L" --max-members " + std::to_wstring(room.members) +
                           L" --preferred-game \"Nintendo Switch\" --preferred-game-id 0100000000010000 --ban-list-file " + quote(bans) + L" --log-file " +
                           quote(Folder(root) + L"\\" + room.id + L".log");
    if (!room.password.empty()) command += L" --password " + quote(room.password);
    // No window and no console; its standard input is nothing, so it stays open until it is closed from here.
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr,
                        Folder(root).c_str(), &startup, &process)) {
        return L"O servidor de sala não abriu (erro " + std::to_wstring(GetLastError()) + L").";
    }
    // A port another program holds makes the server close at once.
    if (WaitForSingleObject(process.hProcess, 800) == WAIT_OBJECT_0) {
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
        return L"A sala \"" + room.name + L"\" fechou logo ao abrir: a porta " + std::to_wstring(room.port) +
               L" deve estar em uso (outra sala, ou um jogo com \"Criar uma sala\" aberto). Veja salas\\" + room.id + L".log.";
    }
    WritePrivateProfileStringW(room.id.c_str(), L"pid", std::to_wstring(process.dwProcessId).c_str(), Ini(root).c_str());
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return L"";
}

void StopRoomServer(const std::wstring& root, const Room& room) {
    if (!RoomRunning(root, room)) return;
    const unsigned long pid = static_cast<unsigned long>(ReadNumber(Ini(root), room.id, L"pid", 0));
    if (HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid)) {
        TerminateProcess(process, 0);
        WaitForSingleObject(process, 3000);
        CloseHandle(process);
    }
    WritePrivateProfileStringW(room.id.c_str(), L"pid", nullptr, Ini(root).c_str());
}

void StartRoomsOn(const std::wstring& root) {
    if (!RoomServerEnabled(root)) return;
    for (const Room& room : LoadRooms(root)) {
        if (room.on) StartRoomServer(root, room);
    }
    for (const Extra extra : {Extra::Classics, Extra::Rooms3ds}) {
        if (ExtraOn(root, extra)) StartExtra(root, extra);
    }
}

namespace {

// The servers beside Eden's rooms, one of each: Ranga's own table server and the 3DS's room server.
struct ExtraInfo {
    const wchar_t* section;   // in salas.ini
    const wchar_t* exe;       // in tools
    const wchar_t* label;     // in messages
    const wchar_t* log;       // in salas
    int port;
};

const ExtraInfo& Info(Extra extra) {
    static const ExtraInfo infos[] = {
        {L"Classicos", L"ranga-sala.exe", L"o servidor dos clássicos", L"classicos.log", 24873},
        {L"Sala3DS", L"azahar-room.exe", L"a sala do 3DS", L"sala-3ds.log", 24874},
    };
    return infos[static_cast<int>(extra)];
}

std::wstring ExtraExe(const std::wstring& root, Extra extra) { return root + L"\\tools\\" + Info(extra).exe; }

}

int ExtraPort(const std::wstring& root, Extra extra) { return std::clamp(ReadNumber(Ini(root), Info(extra).section, L"porta", Info(extra).port), 1, 65535); }

void SetExtraPort(const std::wstring& root, Extra extra, int port) {
    EnsureUnicodeIni(Ini(root));
    WritePrivateProfileStringW(Info(extra).section, L"porta", std::to_wstring(std::clamp(port, 1, 65535)).c_str(), Ini(root).c_str());
}

bool ExtraOn(const std::wstring& root, Extra extra) { return ReadNumber(Ini(root), Info(extra).section, L"ligada", 1) != 0; }

bool ExtraRunning(const std::wstring& root, Extra extra) {
    const unsigned long pid = static_cast<unsigned long>(ReadNumber(Ini(root), Info(extra).section, L"pid", 0));
    if (pid == 0) return false;
    const std::wstring path = ProcessPath(pid);
    return !path.empty() && _wcsicmp(path.c_str(), ExtraExe(root, extra).c_str()) == 0;
}

std::wstring StartExtra(const std::wstring& root, Extra extra) {
    const ExtraInfo& info = Info(extra);
    EnsureUnicodeIni(Ini(root));
    WritePrivateProfileStringW(info.section, L"ligada", L"1", Ini(root).c_str());
    if (ExtraRunning(root, extra)) return L"";
    // A newer server left beside the one that was running (<exe>.novo) takes its place now that it is stopped.
    if (const std::wstring newer = ExtraExe(root, extra) + L".novo"; Exists(newer)) MoveFileExW(newer.c_str(), ExtraExe(root, extra).c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!Exists(ExtraExe(root, extra))) return L"Falta " + std::wstring(info.label) + L" em " + ExtraExe(root, extra) + L".";
    CreateDirectoryW(Folder(root).c_str(), nullptr);
    const int port = ExtraPort(root, extra);
    const std::wstring log = Folder(root) + L"\\" + info.log;
    std::wstring command = L"\"" + ExtraExe(root, extra) + L"\" --port " + std::to_wstring(port);
    if (extra == Extra::Classics) {
        command += L" --log \"" + log + L"\" --atualizacao \"" + root + L"\\salas\\atualizacao\" --perfis \"" + root + L"\\salas\\perfis\"";
    } else {
        // Azahar's room: like Eden's, its ban list is read only when the file starts with its own header.
        const std::wstring bans = Folder(root) + L"\\sala-3ds-banidos.txt";
        if (!Exists(bans)) {
            if (FILE* file = _wfopen(bans.c_str(), L"wb")) {
                std::fputs("CitraRoom-BanList-1\n\n", file);
                std::fclose(file);
            }
        }
        command += L" --room-name \"Sala do Ranga (3DS)\" --max_members 8 --preferred-app \"Nintendo 3DS\" --preferred-app-id 4000000000000"
                   L" --ban-list-file \"" + bans + L"\" --log-file \"" + log + L"\"";
    }
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr,
                        Folder(root).c_str(), &startup, &process)) {
        return std::wstring(info.label) + L" não abriu (erro " + std::to_wstring(GetLastError()) + L").";
    }
    if (WaitForSingleObject(process.hProcess, 800) == WAIT_OBJECT_0) {
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
        return std::wstring(info.label) + L" fechou logo ao abrir: a porta " + std::to_wstring(port) + L" deve estar em uso. Veja salas\\" + info.log + L".";
    }
    WritePrivateProfileStringW(info.section, L"pid", std::to_wstring(process.dwProcessId).c_str(), Ini(root).c_str());
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return L"";
}

void StopExtra(const std::wstring& root, Extra extra) {
    EnsureUnicodeIni(Ini(root));
    WritePrivateProfileStringW(Info(extra).section, L"ligada", L"0", Ini(root).c_str());
    if (!ExtraRunning(root, extra)) return;
    const unsigned long pid = static_cast<unsigned long>(ReadNumber(Ini(root), Info(extra).section, L"pid", 0));
    if (HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid)) {
        TerminateProcess(process, 0);
        WaitForSingleObject(process, 3000);
        CloseHandle(process);
    }
    WritePrivateProfileStringW(Info(extra).section, L"pid", nullptr, Ini(root).c_str());
}

bool RoomsStartWithWindows() { return Exists(StartupShortcut()); }

void SetRoomsStartWithWindows(const std::wstring& root, bool on) {
    const std::wstring shortcut = StartupShortcut();
    if (shortcut.empty()) return;
    if (!on) {
        DeleteFileW(shortcut.c_str());
        return;
    }
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&link)))) return;
    link->SetPath((root + L"\\RangaEmu.exe").c_str());
    link->SetArguments(L"--salas");
    link->SetWorkingDirectory(root.c_str());
    link->SetDescription(L"Liga as salas online do Ranga Emu");
    link->SetShowCmd(SW_SHOWMINNOACTIVE);
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))) {
        file->Save(shortcut.c_str(), TRUE);
        file->Release();
    }
    link->Release();
}

}
