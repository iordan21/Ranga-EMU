#ifndef LAUNCHER_COMMON_HPP
#define LAUNCHER_COMMON_HPP

#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <string>
#include <utility>
#include <vector>

// File, process and game-file helpers shared by the Ranga Emu client. Paths are wide strings everywhere, so a game folder
// may have any characters in its name.
namespace Ranga {

inline constexpr const wchar_t* kAppName = L"Ranga Emu";
inline constexpr const wchar_t* kRuntime = L"ranga_runtime.exe";

bool Exists(const std::wstring& path);
bool IsDirectory(const std::wstring& path);
FILETIME WriteTime(const std::wstring& path);
long long FileSize(const std::wstring& path);
// The lower-case final path of an existing file or folder, through junctions; empty when it cannot be opened.
std::wstring FinalPath(const std::wstring& path);
std::wstring ReadTextLine(const std::wstring& path);
void WriteTextLine(const std::wstring& path, const std::wstring& text);
// Raw UTF-16, so a path with any characters reads back exactly as it was written.
std::wstring ReadPathFile(const std::wstring& path);
void WritePathFile(const std::wstring& path, const std::wstring& text);
std::string ReadBytes(const std::wstring& path);
// At most the last limit bytes of a file another process may still be writing.
std::string ReadTail(const std::wstring& path, size_t limit);
void DeleteFilesIn(const std::wstring& folder);

std::string Utf8(const std::wstring& text);
std::wstring Wide(const std::string& text);

long RunHidden(const std::wstring& command, const std::wstring& directory, const std::wstring& log);
// A directory junction at link that points at target; an existing one that points elsewhere is replaced.
void LinkFolder(const std::wstring& link, const std::wstring& target, const std::wstring& log);

// Every folder and loose file of the game, eboot and sce_module aside, made visible under app0.
void LinkGameFiles(const std::wstring& game, const std::wstring& app0, const std::wstring& log);

// Game files: eboot.bin or eboot.elf and sce_sys; Unity games also have sce_module and Media.
std::wstring GameExecutable(const std::wstring& folder);
bool IsGameFolder(const std::wstring& folder);
bool IsElf(const std::wstring& path);
bool ConvertOrCopy(const std::wstring& from, const std::wstring& to);
void PrepareModules(const std::wstring& game, const std::wstring& prep, std::wstring& failed);

// The profile API writes Unicode only into a file that already starts with a UTF-16 byte order mark.
void EnsureUnicodeIni(const std::wstring& path);
std::wstring ReadIni(const std::wstring& path, const wchar_t* section, const wchar_t* key, const wchar_t* fallback);
std::wstring Lower(std::wstring text);
// What follows the last dot of the file name, lower-case and with the dot; empty when there is none.
std::wstring Extension(const std::wstring& path);
std::wstring FileName(const std::wstring& path);
std::wstring ParentFolder(const std::wstring& path);
// The UTF-8 string value of key in a PlayStation param.sfo (PS3, PSP, PS4: header, 16-byte index entries, key and data
// tables); empty when it is not there.
std::string SfoString(const std::string& sfo, const char* key);

// God of War Sons of Sparta keeps its settings in a save file; these change one integer value in it in place.
int ReadSosSetting(const std::wstring& workspace, const char* key);
bool WriteSosSetting(const std::wstring& workspace, const char* key, int value);

// Posts one F11 to the game window once it presents frames, which is AnyPS5's own borderless fullscreen toggle.
void RequestFullscreen(const PROCESS_INFORMATION& process);

}

#endif
