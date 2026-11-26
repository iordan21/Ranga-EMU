// O que o código de sala do Eden e do Azahar usa do registro deles: aqui, uma linha no terminal (o systemd guarda).
#pragma once
#include <cstdio>
#include <format>
#include <string>
namespace fmt {
using std::format;
}
#define RANGA_LINHA(nivel, ...) (std::puts((std::string(nivel) + std::format(__VA_ARGS__)).c_str()), std::fflush(stdout))
#define LOG_TRACE(classe, ...) ((void)0)
#define LOG_DEBUG(classe, ...) ((void)0)
#define LOG_INFO(classe, ...) RANGA_LINHA("", __VA_ARGS__)
#define LOG_WARNING(classe, ...) RANGA_LINHA("aviso: ", __VA_ARGS__)
#define LOG_ERROR(classe, ...) RANGA_LINHA("erro: ", __VA_ARGS__)
#define LOG_CRITICAL(classe, ...) RANGA_LINHA("erro: ", __VA_ARGS__)
