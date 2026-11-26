#include "Rooms.hpp"
#include "Common.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
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
    if (kRangaAlpha) return;
    if (!RoomServerEnabled(root)) return;
    for (const Room& room : LoadRooms(root)) {
        if (room.on) StartRoomServer(root, room);
    }
    for (const Extra extra : {Extra::Classics, Extra::Rooms3ds, Extra::Ps3}) {
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
        {L"ServidorPS3", L"rpcn\\rpcn.exe", L"o servidor do PS3", L"servidor-ps3.log", 31313},
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

int XboxHubPort(const std::wstring& root) { return std::clamp(ReadNumber(Ini(root), L"Classicos", L"hub_xbox", 24875), 1, 65535); }

bool ExtraLan(const std::wstring& root, Extra extra) { return ReadNumber(Ini(root), Info(extra).section, L"lan", 0) != 0; }

void SetExtraLan(const std::wstring& root, Extra extra, bool on) {
    EnsureUnicodeIni(Ini(root));
    WritePrivateProfileStringW(Info(extra).section, L"lan", on ? L"1" : L"0", Ini(root).c_str());
}

void ShareShaderCaches(const std::wstring& root) {
    // Eden keeps each game's cache in bios\retroarch\eden\shader\<title id>\; vulkan.bin is the part any GPU can use
    // (vulkan_pipelines.bin is this GPU's own). A newer or bigger one is copied over the shared one.
    const std::wstring from = root + L"\\bios\\retroarch\\eden\\shader", to = root + L"\\salas\\shaders";
    CreateDirectoryW(to.c_str(), nullptr);
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((from + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring id = found.cFileName;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || id.size() != 16) continue;
        const std::wstring source = from + L"\\" + id + L"\\vulkan.bin", target = to + L"\\" + Lower(id) + L".bin";
        unsigned long long sourceStamp = 0, sourceSize = 0, targetStamp = 0, targetSize = 0;
        if (!FileStamp(source, sourceStamp, sourceSize) || sourceSize == 0) continue;
        if (FileStamp(target, targetStamp, targetSize) && targetSize >= sourceSize && targetStamp >= sourceStamp) continue;
        CopyFileW(source.c_str(), target.c_str(), FALSE);
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

int ProbeRoom(const std::string& address, int defaultPort, bool tcp) {
    std::string host = address;
    int port = defaultPort;
    if (const size_t colon = host.rfind(':'); colon != std::string::npos) {
        port = std::atoi(host.c_str() + colon + 1);
        host.resize(colon);
    }
    while (!host.empty() && host.front() == ' ') host.erase(0, 1);
    while (!host.empty() && host.back() == ' ') host.pop_back();
    if (host.empty() || port <= 0 || port > 65535) return -1;
#ifdef RANGA_ALPHA
    return -1;
#endif
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = tcp ? SOCK_STREAM : SOCK_DGRAM;
    addrinfo* found = nullptr;
    int result = -1;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) == 0 && found != nullptr) {
        const SOCKET sock = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
        if (sock != INVALID_SOCKET) {
            const ULONGLONG started = GetTickCount64();
            if (tcp) {
                u_long nonBlocking = 1;
                ioctlsocket(sock, FIONBIO, &nonBlocking);
                connect(sock, found->ai_addr, static_cast<int>(found->ai_addrlen));
                fd_set writes, errors;
                FD_ZERO(&writes);
                FD_ZERO(&errors);
                FD_SET(sock, &writes);
                FD_SET(sock, &errors);
                timeval wait{3, 0};
                if (select(0, nullptr, &writes, &errors, &wait) > 0 && FD_ISSET(sock, &writes)) result = static_cast<int>(GetTickCount64() - started);
            } else {
                // ENet's connect command (no checksum, as Eden's and Azahar's rooms run): peer 0x0FFF with the sent
                // time, command 2 with "acknowledge", then the connect fields. Any datagram back is an answer.
                unsigned char packet[4 + 4 + 44] = {};
                const unsigned short sent = static_cast<unsigned short>(GetTickCount64() & 0xFFFF);
                const unsigned char head[] = {0x8F, 0xFF, static_cast<unsigned char>(sent >> 8), static_cast<unsigned char>(sent), 0x82, 0xFF, 0x00, 0x01};
                std::memcpy(packet, head, sizeof(head));
                const unsigned char body[] = {0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x05, 0x78, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x01,
                                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x13, 0x88, 0x00, 0x00, 0x00, 0x02,
                                              0x00, 0x00, 0x00, 0x02, 0x12, 0x34, 0xAB, 0xCD, 0x00, 0x00, 0x00, 0x00};
                std::memcpy(packet + 8, body, sizeof(body));
                sendto(sock, reinterpret_cast<const char*>(packet), static_cast<int>(sizeof(packet)), 0, found->ai_addr, static_cast<int>(found->ai_addrlen));
                fd_set reads;
                FD_ZERO(&reads);
                FD_SET(sock, &reads);
                timeval wait{3, 0};
                char answer[512];
                if (select(0, &reads, nullptr, nullptr, &wait) > 0 && recv(sock, answer, sizeof(answer), 0) > 0) result = static_cast<int>(GetTickCount64() - started);
            }
            closesocket(sock);
        }
        freeaddrinfo(found);
    }
    WSACleanup();
    return result;
}

namespace {

struct KindInfo {
    const char* key;
    int port;
};
constexpr KindInfo kKinds[] = {{"switch", 24872}, {"3ds", 24874}, {"classicos", 24873}, {"xbox", 24875}, {"ps3", 31313}};

// A plain host name or IPv4, with ":port" when port is asked for; empty when it is anything else.
std::string PlainAddress(std::string text, bool port) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\r' || text.back() == '\n')) text.pop_back();
    while (!text.empty() && text.front() == ' ') text.erase(0, 1);
    if (text.size() > 120) return "";
    const size_t colon = text.find(':');
    if (port != (colon != std::string::npos)) return "";
    const std::string host = text.substr(0, colon);
    if (host.empty() || host.front() == '-' || host.front() == '.') return "";
    for (const char c : host) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-')) return "";
    if (port) {
        const std::string number = text.substr(colon + 1);
        if (number.empty() || number.size() > 5 || number.find_first_not_of("0123456789") != std::string::npos) return "";
        if (const int value = std::atoi(number.c_str()); value < 1 || value > 65535) return "";
    }
    return text;
}

}

RoomServers ReadRoomServers(const std::string& text) {
    RoomServers servers;
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(start, end - start);
        start = end + 1;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string key = line.substr(0, tab), value = line.substr(tab + 1);
        if (key == "servidor") servers.host = PlainAddress(value, false);
        for (int i = 0; i < 5; ++i) if (key == kKinds[i].key) servers.reserve[i] = PlainAddress(value, true);
    }
    return servers;
}

std::string WriteRoomServers(const RoomServers& servers) {
    std::string text = "servidor\t" + servers.host + "\n";
    for (int i = 0; i < 5; ++i) if (!servers.reserve[i].empty()) text += std::string(kKinds[i].key) + "\t" + servers.reserve[i] + "\n";
    return text;
}

RoomServers LoadRoomServers(const std::wstring& root) {
    const std::wstring ini = root + L"\\engines.ini";
    std::string text = "servidor\t" + Utf8(ReadIni(ini, L"Salas", L"servidor", L"")) + "\n";
    for (const KindInfo& kind : kKinds) text += std::string(kind.key) + "\t" + Utf8(ReadIni(ini, L"Salas", Wide(kind.key).c_str(), L"")) + "\n";
    return ReadRoomServers(text);
}

void SaveRoomServers(const std::wstring& root, const RoomServers& servers) {
    const RoomServers now = LoadRoomServers(root);
    if (WriteRoomServers(now) == WriteRoomServers(servers)) return;
    const std::wstring ini = root + L"\\engines.ini";
    EnsureUnicodeIni(ini);
    const auto put = [&](const wchar_t* key, const std::string& value) {
        WritePrivateProfileStringW(L"Salas", key, value.empty() ? nullptr : Wide(value).c_str(), ini.c_str());
    };
    put(L"servidor", servers.host);
    for (int i = 0; i < 5; ++i) put(Wide(kKinds[i].key).c_str(), servers.reserve[i]);
}

std::string RoomServerAddress(const RoomServers& servers, RoomKind kind) {
    return servers.host.empty() ? "" : servers.host + ":" + std::to_string(kKinds[static_cast<int>(kind)].port);
}

std::vector<std::string> RoomWays(const RoomServers& servers, RoomKind kind, const std::string& address) {
    const std::string own = RoomServerAddress(servers, kind), &reserve = servers.reserve[static_cast<int>(kind)];
    const auto full = [&](const std::string& text) {
        return Lower(Wide(text.find(':') == std::string::npos ? text + ":" + std::to_string(kKinds[static_cast<int>(kind)].port) : text));
    };
    if (own.empty() || address.empty() || (full(address) != full(own) && (reserve.empty() || full(address) != full(reserve)))) return {address};
    std::vector<std::string> ways{own};
    if (!reserve.empty()) ways.push_back(reserve);
    return ways;
}

std::vector<std::wstring> LanAddresses() {
    std::vector<std::wstring> found;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer(size);
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size) == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    }
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size) != NO_ERROR) return found;
    for (auto* adapter = list; adapter != nullptr; adapter = adapter->Next) {
        // Only cards that are up and lead somewhere (a gateway): not the loopback, nor virtual ones without a way out.
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || adapter->FirstGatewayAddress == nullptr) continue;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
            const auto* ip = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
            const unsigned char* b = reinterpret_cast<const unsigned char*>(&ip->sin_addr);
            // Home networks: 10.x, 172.16-31.x, 192.168.x.
            if (!(b[0] == 10 || (b[0] == 172 && b[1] >= 16 && b[1] <= 31) || (b[0] == 192 && b[1] == 168))) continue;
            found.push_back(std::to_wstring(b[0]) + L"." + std::to_wstring(b[1]) + L"." + std::to_wstring(b[2]) + L"." + std::to_wstring(b[3]));
        }
    }
    return found;
}

bool ExtraOn(const std::wstring& root, Extra extra) { return ReadNumber(Ini(root), Info(extra).section, L"ligada", extra == Extra::Ps3 ? 0 : 1) != 0; }

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
    // RPCN works in its own folder (rpcn.cfg, its database in db\, its TLS certificate), with the port in rpcn.cfg.
    // The certificate is made here the first time (self-signed; RPCS3 doesn't check it), and never leaves this PC.
    std::wstring folder = Folder(root);
    if (extra == Extra::Ps3) {
        folder = ExtraExe(root, extra).substr(0, ExtraExe(root, extra).find_last_of(L'\\'));
        command = L"\"" + ExtraExe(root, extra) + L"\"";
        if (!Exists(folder + L"\\cert.pem") || !Exists(folder + L"\\key.pem")) RunHidden(command + L" --cert-gen", folder, log);
    }
    if (extra == Extra::Classics) {
        // Open to the home network too when asked (lan=1); otherwise only this PC and its playit tunnel reach it.
        if (ExtraLan(root, extra)) command += L" --lan";
        // The Xbox's System Link hub rides along, on its own UDP port.
        command += L" --hub-xbox " + std::to_wstring(XboxHubPort(root));
        // The Switch games' shader caches this PC hands out (ShareShaderCaches keeps them up to date).
        CreateDirectoryW((root + L"\\salas\\shaders").c_str(), nullptr);
        command += L" --shaders \"" + root + L"\\salas\\shaders\"";
        command += L" --log \"" + log + L"\" --atualizacao \"" + root + L"\\salas\\atualizacao\" --perfis \"" + root + L"\\salas\\perfis\"";
        // The group's code, when the friends set one (letters and digits only, see PlayerProfile.hpp GroupCode).
        std::wstring group;
        for (const wchar_t c : ReadIni(root + L"\\engines.ini", L"Ranga", L"grupo", L"")) if (iswalnum(c) || c == L'-' || c == L'_') group += c;
        if (!group.empty()) command += L" --grupo " + group;
    } else if (extra == Extra::Rooms3ds) {
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
                        folder.c_str(), &startup, &process)) {
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
