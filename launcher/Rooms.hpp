#ifndef LAUNCHER_ROOMS_HPP
#define LAUNCHER_ROOMS_HPP

#include <string>
#include <vector>

namespace Ranga {

// The Switch's online rooms this PC keeps open for everyone: each one is Eden's room server (tools\eden-room.exe),
// running on its own, without any game open, on a port a tunnel (playit) can point at. Everyone, the owner of the PC
// included, joins it with "Entrar em uma sala"; anyone in it creates the game's own group. Kept in salas.ini beside
// this program; each room's ban list and log in salas\.
struct Room {
    std::wstring id;       // the section in salas.ini
    std::wstring name;
    std::wstring password; // empty: anyone with the address gets in
    int port = 24872;
    int members = 16;      // at most 16, Eden's limit
    bool on = false;       // kept open: started with Ranga and, when asked, with Windows
};

// Only the PC that hosts runs room servers: everyone else just joins an address. Off unless enabled once with
// RangaEmu.exe --servidor (engines.ini [Ranga] servidor=1); while off, the rooms section is hidden and no room starts,
// with Ranga or with Windows, even if salas.ini came along in a copy.
bool RoomServerEnabled(const std::wstring& root);
void SetRoomServerEnabled(const std::wstring& root, bool on);

std::vector<Room> LoadRooms(const std::wstring& root);
void SaveRoom(const std::wstring& root, const Room& room);
void RemoveRoom(const std::wstring& root, const std::wstring& id);
// A new room on the next port no other room uses, saved, not yet started.
Room AddRoom(const std::wstring& root);

// Whether the room's server is running now (the process it was started as, still Ranga's eden-room.exe).
bool RoomRunning(const std::wstring& root, const Room& room);
// Starts the server in the background, with no window; empty on success, otherwise why not.
std::wstring StartRoomServer(const std::wstring& root, const Room& room);
void StopRoomServer(const std::wstring& root, const Room& room);
// Starts every room marked on that is not running. Used when Ranga opens and by RangaEmu.exe --salas (Windows' start).
void StartRoomsOn(const std::wstring& root);

// The servers beside Eden's rooms, one of each, started and stopped with the rooms. In salas.ini, a section each with
// porta and ligada (on unless turned off):
//   Classics  [Classicos] tools\ranga-sala.exe: the table server of online play for the consoles whose cores have none
//             (NES to PS2, launcher/net), TCP 24873, one for every game.
//   Rooms3ds  [Sala3DS] tools\azahar-room.exe: Azahar's room server for the 3DS's local wireless, UDP 24874.
//   Ps3       [ServidorPS3] tools\rpcn\rpcn.exe: RPCN, the PS3's online server (RPCS3's, open source), TCP 31313 and
//             UDP 3657, without e-mail: the PS3 core makes each player's account on first use. Off unless turned on.
enum class Extra { Classics, Rooms3ds, Ps3 };
int ExtraPort(const std::wstring& root, Extra extra);
void SetExtraPort(const std::wstring& root, Extra extra, int port);
bool ExtraOn(const std::wstring& root, Extra extra);
// The classics' server open to the home network as well (salas.ini lan=1, ranga-sala --lan); off: this PC and its
// tunnel only. The 3DS's and the Switch's rooms always listen on every network.
bool ExtraLan(const std::wstring& root, Extra extra);
void SetExtraLan(const std::wstring& root, Extra extra, bool on);
// The Xbox's System Link hub (UDP), run by the classics' server (salas.ini [Classicos] hub_xbox, 24875).
int XboxHubPort(const std::wstring& root);
// Whether a room answers, in milliseconds (-1: no answer in 3 s). address is "host:port" or "host" (then
// defaultPort). Switch and 3DS rooms (ENet, UDP) get a connection request, which a room answers at once; the classics'
// server (tcp) a connection. Blocks: run it off the drawing thread.
int ProbeRoom(const std::string& address, int defaultPort, bool tcp);
// This PC's addresses on home networks (192.168.x, 10.x, 172.16-31.x), for the friends in the same house.
std::vector<std::wstring> LanAddresses();
bool ExtraRunning(const std::wstring& root, Extra extra);
std::wstring StartExtra(const std::wstring& root, Extra extra);
void StopExtra(const std::wstring& root, Extra extra);

// The shortcut in the user's Startup folder that runs RangaEmu.exe --salas when Windows starts.
bool RoomsStartWithWindows();
void SetRoomsStartWithWindows(const std::wstring& root, bool on);

}

#endif
