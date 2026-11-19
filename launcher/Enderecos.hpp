#pragma once

// Os endereços do PC que hospeda (túneis do playit): a fonte da atualização automática e a sala privada do Switch.
// Os de verdade ficam em Enderecos.local.hpp, que não vai para o git (assim o código pode ir para o GitHub sem
// mostrar onde está o servidor). Sem ele, o Ranga compila com os dois vazios: a atualização e a sala privada usam o que
// estiver no engines.ini ([Ranga] atualizacao) ou o que a pessoa digitar.
#if __has_include("Enderecos.local.hpp")
#include "Enderecos.local.hpp"
#else
#define RANGA_FONTE_ATUALIZACAO L""
#define RANGA_SALA_SWITCH_PRIVADA ""
#endif
