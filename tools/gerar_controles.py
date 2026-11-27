"""Gera launcher/ControleDb.cpp a partir de 3rdparty/SDL_GameControllerDB/gamecontrollerdb.txt.

A lista da comunidade (https://github.com/mdqinc/SDL_GameControllerDB, licença zlib) diz como ler os botões de
milhares de controles genéricos que o SDL2 não conhece sozinho. Só entram as linhas do Windows, embutidas no
RangaEmu.exe e no ranga_retro.exe, para não haver arquivo solto para perder.

Uso: python -I tools/gerar_controles.py
"""
import pathlib

raiz = pathlib.Path(__file__).resolve().parent.parent
origem = raiz / "3rdparty" / "SDL_GameControllerDB" / "gamecontrollerdb.txt"
destino = raiz / "launcher" / "ControleDb.cpp"

linhas = [l.strip() for l in origem.read_text(encoding="utf-8").splitlines()]
linhas = [l for l in linhas if l and not l.startswith("#") and "platform:Windows" in l]


def literal(texto: str) -> str:
    return '"' + texto.replace("\\", "\\\\").replace('"', '\\"') + '\\n"'


partes = [
    "// Gerado por tools/gerar_controles.py a partir de 3rdparty/SDL_GameControllerDB (licença zlib). Não edite à mão.",
    '#include "ControleDb.hpp"',
    "",
    "#include <SDL.h>",
    "",
    "namespace Ranga {",
    "",
    "namespace {",
    "",
    f"// {len(linhas)} controles do Windows.",
    "const char kMappings[] =",
]
partes += ["    " + literal(l) for l in linhas]
partes[-1] += ";"
partes += [
    "",
    "}",
    "",
    "int AddKnownPads() {",
    "    return SDL_GameControllerAddMappingsFromRW(SDL_RWFromConstMem(kMappings, sizeof(kMappings) - 1), 1);",
    "}",
    "",
    "}",
    "",
]
destino.write_text("\n".join(partes), encoding="utf-8", newline="\n")
print(f"{len(linhas)} controles em {destino.relative_to(raiz)}")
