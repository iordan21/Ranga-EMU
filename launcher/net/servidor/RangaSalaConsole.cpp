// ranga-sala-switch e ranga-sala-3ds: só o servidor de sala do Eden (Switch) e do Azahar (3DS), para uma máquina Linux
// pequena. A sala em si é o código original deles (src/network/room.cpp e packet.cpp, mais a ENet), sem mudança; este
// arquivo troca o programa deles, que puxaria o emulador inteiro para compilar.
//
//   ranga-sala-switch [--nome "Sala do Ranga"] [--porta 24872] [--maximo 16]
//
// A senha vem da variável RANGA_SENHA, para não aparecer na lista de processos. Fecha com SIGTERM ou Ctrl+C.
// Compilado com -DRANGA_SWITCH ou -DRANGA_3DS (compilar.sh).

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "enet/enet.h"
#include "network/room.h"
#include "network/verify_user.h"

namespace {

std::atomic<bool> g_close{false};
void Close(int) { g_close = true; }

}

int main(int argc, char** argv) {
#ifdef RANGA_SWITCH
    std::string name = "Sala do Ranga", password;
    int port = 24872, members = 16;
#else
    std::string name = "Sala do Ranga (3DS)", password;
    int port = 24874, members = 8;
#endif
    if (const char* secret = std::getenv("RANGA_SENHA")) password = secret;
    for (int i = 1; i < argc; i += 2) {
        const std::string key = argv[i], value = i + 1 < argc ? argv[i + 1] : "";
        if (key == "--nome" && !value.empty()) name = value;
        else if (key == "--porta" && !value.empty()) port = std::atoi(value.c_str());
        else if (key == "--maximo" && !value.empty()) members = std::atoi(value.c_str());
        else {
            std::fprintf(stderr, "opcao desconhecida ou sem valor: %s\n", key.c_str());
            return 2;
        }
    }
    if (port <= 0 || port > 65535) {
        std::fprintf(stderr, "porta invalida\n");
        return 2;
    }
    members = std::clamp(members, 2, 16);
    std::signal(SIGTERM, Close);
    std::signal(SIGINT, Close);
    if (enet_initialize() != 0) {
        std::fprintf(stderr, "a rede (ENet) nao abriu\n");
        return 1;
    }
    int result = 0;
    {
        Network::Room room;
        auto verify = std::make_unique<Network::VerifyUser::NullBackend>();
#ifdef RANGA_SWITCH
        const bool open = room.Create(name, "", "", static_cast<u16>(port), password, static_cast<u32>(members), "",
                                      AnnounceMultiplayerRoom::GameInfo{"Nintendo Switch", 0x0100000000010000ULL, ""}, std::move(verify), {});
#else
        const bool open = room.Create(name, "", "", static_cast<u16>(port), password, static_cast<u32>(members), "", "Nintendo 3DS", 0x4000000000000ULL,
                                      std::move(verify), {});
#endif
        if (!open) {
            std::fprintf(stderr, "a porta UDP %d esta em uso\n", port);
            result = 1;
        } else {
            std::printf("%s aberta na porta UDP %d (versao da rede %u, ate %d jogadores)\n", name.c_str(), port, static_cast<unsigned>(Network::network_version), members);
            std::fflush(stdout);
            while (!g_close && room.GetState() == Network::Room::State::Open) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            room.Destroy();
        }
    }
    enet_deinitialize();
    return result;
}
