#!/usr/bin/env bash
# Compila as salas do Switch, do 3DS e dos clássicos para a pasta dada. Usado pelo instalar.sh; roda também no Git Bash
# com o MinGW, para conferir antes de mandar.
#   bash compilar.sh <pasta-de-saida>
set -euo pipefail
SAIDA="${1:?Uso: bash compilar.sh <pasta-de-saida>}"
mkdir -p "$SAIDA"
SAIDA="$(cd "$SAIDA" && pwd)"
cd "$(dirname "$0")"
OBJ="$(mktemp -d)"
trap 'rm -rf "$OBJ"' EXIT
case "$(uname -s)" in
    MINGW* | MSYS*)
        PLATAFORMA=win32 EXT=.exe
        LIBS=(-lws2_32 -lwinmm -static)
        DEFS=()
        ;;
    *)
        # O que o CMake da ENet confere e encontra em qualquer Linux com glibc.
        PLATAFORMA=unix EXT=
        LIBS=(-pthread)
        DEFS=(-DHAS_FCNTL=1 -DHAS_POLL=1 -DHAS_GETADDRINFO=1 -DHAS_GETNAMEINFO=1 -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 -DHAS_MSGHDR_FLAGS=1 -DHAS_SOCKLEN_T=1)
        ;;
esac
for f in callbacks compress host list packet peer protocol "$PLATAFORMA"; do
    gcc -O2 -w "${DEFS[@]}" -Ienet/include -c "enet/$f.c" -o "$OBJ/enet-$f.o"
done
sala() { # pasta definição nome
    g++ -O2 -std=c++20 -w "-D$2" "-I$1" -Ifontes/calco -Ienet/include RangaSalaConsole.cpp "$1/network/room.cpp" "$1/network/packet.cpp" \
        "$1/network/verify_user.cpp" "$OBJ"/enet-*.o -o "$SAIDA/$3$EXT" "${LIBS[@]}"
}
sala switch RANGA_SWITCH ranga-sala-switch
sala 3ds RANGA_3DS ranga-sala-3ds
g++ -O2 -std=c++17 -Ifontes fontes/RangaSala.cpp -o "$SAIDA/ranga-sala$EXT" "${LIBS[@]}"
echo "Compilado em $SAIDA"
