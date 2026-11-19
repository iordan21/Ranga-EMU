#ifndef LAUNCHER_LOBBY_HPP
#define LAUNCHER_LOBBY_HPP

// The Switch's public rooms: the list Eden's own lobby keeps (api.ynet-fun.xyz/lobby, the web_api_url Eden ships
// with), who is in each room and what each one is playing, and how far each room is (ping). The 3DS has no such list:
// Azahar keeps none on purpose.

#include <cstdint>
#include <string>
#include <vector>

namespace Ranga {

struct LobbyPlayer {
    std::string nickname, gameName;
    uint64_t gameId = 0;
};

struct LobbyRoom {
    std::string name, description, address, preferredGameName;
    int port = 0, maxPlayers = 0, netVersion = 0;
    bool hasPassword = false;
    uint64_t preferredGameId = 0;
    std::vector<LobbyPlayer> players;
    int ping = -1; // ms; -1 when the room's machine does not answer ping
};

// The rooms the list has now, only those whose version matches the Eden in Ranga; false and why when it failed.
bool FetchSwitchLobby(std::vector<LobbyRoom>& rooms, std::string& error);
// ICMP echo to an IPv4 address, in ms; -1 when there is no answer within timeoutMs.
int PingAddress(const std::string& address, int timeoutMs);
// A Switch game's title id, read from the "[0100XXXXXXXXXXXX]" its file name usually carries; 0 when it has none.
uint64_t SwitchTitleId(const std::wstring& path);
// Lower-case letters and digits only, words separated by one space: for matching game names loosely.
std::string LooseTitle(const std::string& title);

}

#endif
