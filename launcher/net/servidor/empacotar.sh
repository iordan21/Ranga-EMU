#!/usr/bin/env bash
# Junta em um .tar.gz tudo que a máquina Linux precisa para compilar as salas: o servidor dos clássicos, os arquivos de
# sala do Eden e do Azahar (do código que o Ranga usa, para a versão da rede ser a mesma) e a ENet. Roda no Git Bash.
#   bash launcher/net/servidor/empacotar.sh [saida.tar.gz]
set -euo pipefail
AQUI="$(cd "$(dirname "$0")" && pwd)"
RAIZ="$(cd "$AQUI/../../../.." && pwd)"
SAIDA="${1:-$RAIZ/ranga-servidor.tar.gz}"
EDEN="$RAIZ/eden-libretro/eden/src"
AZAHAR="$RAIZ/azahar-libretro/azahar/src"
ENET="$(ls -d "$RAIZ"/eden-libretro/eden/.cache/cpm/enet/*/ | head -1)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
D="$T/ranga-servidor"
mkdir -p "$D/enet" "$D/fontes/calco"
cp "$AQUI/instalar.sh" "$AQUI/compilar.sh" "$AQUI/copia-rpcn.sh" "$AQUI/RangaSalaConsole.cpp" "$AQUI/LEIA-ME.md" "$D/"
cp -r "$AQUI/calco/." "$D/fontes/calco/"
cp "$AQUI/../RangaSala.cpp" "$AQUI/../RangaNet.hpp" "$D/fontes/"
for f in callbacks.c compress.c host.c list.c packet.c peer.c protocol.c unix.c win32.c include LICENSE; do cp -r "$ENET/$f" "$D/enet/"; done
copiar() { # origem destino arquivos...
    local de="$1" para="$2" f
    shift 2
    for f in "$@"; do
        mkdir -p "$para/$(dirname "$f")"
        cp "$de/$f" "$para/$f"
    done
}
REDE=(network/room.cpp network/room.h network/packet.cpp network/packet.h network/verify_user.cpp network/verify_user.h common/common_types.h)
copiar "$EDEN" "$D/switch" "${REDE[@]}" common/announce_multiplayer_room.h common/socket_types.h web_service/web_result.h
copiar "$AZAHAR" "$D/3ds" "${REDE[@]}"
cp "$EDEN/../LICENSE.txt" "$D/switch/LICENSE.txt"
cp "$AZAHAR/../LICENSE.txt" "$D/3ds/LICENSE.txt"
# Tudo com fim de linha de Linux: um .sh com CRLF não roda lá.
find "$D" -type f -exec sed -i 's/\r$//' {} +
tar -C "$T" -czf "$SAIDA" ranga-servidor
echo "Pronto: $SAIDA ($(du -h "$SAIDA" | cut -f1))"
