#ifndef LAUNCHER_ALPHAEXTRA_HPP
#define LAUNCHER_ALPHAEXTRA_HPP

// Ranga Alpha's private consoles live in launcher/alpha, which is kept on the owner's PC and never published. CMake
// adds it to the RangaAlpha target (and defines RANGA_ALPHA_EXTRA) only when the folder is there; without it Ranga
// Alpha builds with the consoles that are public, and the normal Ranga never sees any of this.

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#ifdef RANGA_ALPHA_EXTRA
#include "alpha/Textos.hpp"
#else
// Extra folder kinds named on the Alpha's "add a game" texts (", Console" and " ou do Console (com arquivo)").
#define RANGA_ALPHA_EXTRA_LIST ""
#define RANGA_ALPHA_EXTRA_FOLDERS ""
#endif

namespace Ranga {

struct Game;
struct Options;
struct Engine;
struct Detected;
struct GraphicsOption;

namespace AlphaExtra {
#ifdef RANGA_ALPHA_EXTRA
bool IsSystem(const std::wstring& id);
// The emulator's exe when none was chosen on the Emulators page.
std::wstring DefaultExe(const std::wstring& root, const std::wstring& id);
// true when the folder is one of these consoles' games (found tells which); false leaves it to the others.
bool DetectFolder(const std::wstring& folder, Detected& found);
// true when system is one of these consoles; rom is then the file that marks the game ("" when missing).
bool GameRom(const std::wstring& system, const std::wstring& path, std::wstring& rom);
const std::vector<GraphicsOption>* Graphics(const std::wstring& system);
const char* GraphicsNote(const std::wstring& system);
// The emulator's settings for the game about to start (on), or taken away again (off).
void Environment(const std::wstring& root, const Game& game, const Options& options, bool on);
// Work done before the game opens; skip stops it, skippable tells the window it can, progress is shown as it goes.
void Prepare(const std::wstring& root, const Game& game, const Options& options, const Engine& engine, const std::wstring& rom,
             const std::wstring& logs, std::atomic<bool>& skip, std::atomic<bool>& skippable,
             const std::function<void(const std::wstring&)>& progress);
#else
inline bool IsSystem(const std::wstring&) { return false; }
#endif
}  // namespace AlphaExtra

}  // namespace Ranga

#endif
