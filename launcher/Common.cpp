#include "Common.hpp"

#include <objidl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace Ranga {

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool IsDirectory(const std::wstring& path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

FILETIME WriteTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return FILETIME{};
    return data.ftLastWriteTime;
}

long long FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return -1;
    return (static_cast<long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

std::wstring FinalPath(const std::wstring& path) {
    HANDLE handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return L"";
    wchar_t buffer[2048];
    const DWORD length = GetFinalPathNameByHandleW(handle, buffer, 2048, VOLUME_NAME_DOS);
    CloseHandle(handle);
    if (length == 0 || length >= 2048) return L"";
    std::wstring result = buffer;
    for (auto& character : result) character = static_cast<wchar_t>(towlower(character));
    return result;
}

std::wstring ReadTextLine(const std::wstring& path) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"r") != 0 || file == nullptr) return L"";
    wchar_t line[256] = {};
    fgetws(line, 256, file);
    fclose(file);
    std::wstring text = line;
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) text.pop_back();
    return text;
}

void WriteTextLine(const std::wstring& path, const std::wstring& text) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"w") != 0 || file == nullptr) return;
    fputws(text.c_str(), file);
    fclose(file);
}

std::wstring ReadPathFile(const std::wstring& path) {
    const std::string bytes = ReadBytes(path);
    return std::wstring(reinterpret_cast<const wchar_t*>(bytes.data()), bytes.size() / sizeof(wchar_t));
}

void WritePathFile(const std::wstring& path, const std::wstring& text) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) return;
    fwrite(text.data(), sizeof(wchar_t), text.size(), file);
    fclose(file);
}

std::string ReadBytes(const std::wstring& path) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) return {};
    std::string data;
    char buffer[8192];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0) data.append(buffer, count);
    fclose(file);
    return data;
}

std::string ReadTail(const std::wstring& path, size_t limit) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    const auto start = size.QuadPart > static_cast<LONGLONG>(limit) ? size.QuadPart - static_cast<LONGLONG>(limit) : 0;
    LARGE_INTEGER position{};
    position.QuadPart = start;
    SetFilePointerEx(file, position, nullptr, FILE_BEGIN);
    std::string data(static_cast<size_t>(size.QuadPart - start), '\0');
    DWORD read = 0;
    if (!data.empty() && !ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr)) read = 0;
    CloseHandle(file);
    data.resize(read);
    // A cut first line is dropped.
    if (start > 0) {
        const auto newline = data.find('\n');
        data.erase(0, newline == std::string::npos ? data.size() : newline + 1);
    }
    return data;
}

void DeleteFilesIn(const std::wstring& folder) {
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((folder + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) DeleteFileW((folder + L"\\" + found.cFileName).c_str());
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

long RunHidden(const std::wstring& command, const std::wstring& directory, const std::wstring& log) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE output = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (output != INVALID_HANDLE_VALUE) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = output;
        startup.hStdError = output;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION process{};
    std::wstring mutableCommand = command;
    const BOOL started = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process);
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    if (!started) return -1;
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return static_cast<long>(code);
}

void LinkFolder(const std::wstring& link, const std::wstring& target, const std::wstring& log) {
    if (!Exists(target)) return;
    if (Exists(link)) {
        if (FinalPath(link) == FinalPath(target)) return;
        RemoveDirectoryW(link.c_str());
        if (Exists(link)) return;
    }
    const auto parent = link.substr(0, link.find_last_of(L'\\'));
    CreateDirectoryW(parent.c_str(), nullptr);
    RunHidden(L"cmd.exe /c mklink /J \"" + link + L"\" \"" + target + L"\"", parent, log);
}

void LinkGameFiles(const std::wstring& game, const std::wstring& app0, const std::wstring& log) {
    CreateDirectoryW(app0.c_str(), nullptr);
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((game + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = found.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring source = game + L"\\" + name, target = app0 + L"\\" + name;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            // sce_module in app0 holds the relinked modules, never the game's own.
            if (_wcsicmp(name.c_str(), L"sce_module") != 0) LinkFolder(target, source, log);
            continue;
        }
        if (_wcsnicmp(name.c_str(), L"eboot.", 6) == 0) continue;
        if (Exists(target) && FileSize(target) == FileSize(source)) {
            const FILETIME a = WriteTime(target), b = WriteTime(source);
            if (CompareFileTime(&a, &b) == 0) continue;
        }
        DeleteFileW(target.c_str());
        // A hard link costs nothing but needs the same drive; a game on another drive gets its loose files copied.
        if (!CreateHardLinkW(target.c_str(), source.c_str(), nullptr)) CopyFileW(source.c_str(), target.c_str(), FALSE);
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

std::wstring GameExecutable(const std::wstring& folder) {
    if (Exists(folder + L"\\eboot.bin")) return folder + L"\\eboot.bin";
    if (Exists(folder + L"\\eboot.elf")) return folder + L"\\eboot.elf";
    return L"";
}

bool IsGameFolder(const std::wstring& folder) {
    return !folder.empty() && !GameExecutable(folder).empty() && IsDirectory(folder + L"\\sce_sys");
}

bool IsElf(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    unsigned char magic[4] = {};
    DWORD read = 0;
    const bool ok = ReadFile(file, magic, sizeof(magic), &read, nullptr) && read == 4 && magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
    CloseHandle(file);
    return ok;
}

namespace {

// A fake-signed SELF, as dumped from a console, holds the plain ELF in pieces; this puts them back in place.
bool ExtractSelf(const std::wstring& selfPath, const std::wstring& elfPath) {
    HANDLE in = CreateFileW(selfPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (in == INVALID_HANDLE_VALUE) return false;
    auto readAt = [&](unsigned long long offset, void* buffer, DWORD size) {
        LARGE_INTEGER position;
        position.QuadPart = static_cast<LONGLONG>(offset);
        DWORD got = 0;
        return SetFilePointerEx(in, position, nullptr, FILE_BEGIN) && ReadFile(in, buffer, size, &got, nullptr) && got == size;
    };
    struct Entry { unsigned long long props, offset, fileSize, memSize; };
    struct Header { unsigned type, flags; unsigned long long offset, vaddr, paddr, fileSize, memSize, align; };
    unsigned char head[0x20] = {};
    bool ok = readAt(0, head, sizeof(head)) && head[0] == 0x4f && head[1] == 0x15 && head[2] == 0x3d && head[3] == 0x1d;
    const unsigned entryCount = ok ? *reinterpret_cast<unsigned short*>(head + 0x18) : 0;
    std::vector<Entry> entries(entryCount);
    unsigned char elfHead[64] = {};
    ok = ok && entryCount != 0 && entryCount < 4096 && readAt(0x20, entries.data(), entryCount * sizeof(Entry)) &&
         readAt(0x20 + entryCount * sizeof(Entry), elfHead, sizeof(elfHead)) && elfHead[0] == 0x7f && elfHead[1] == 'E';
    std::vector<Header> headers;
    unsigned long long tableOffset = 0;
    if (ok) {
        tableOffset = *reinterpret_cast<unsigned long long*>(elfHead + 0x20);
        const unsigned count = *reinterpret_cast<unsigned short*>(elfHead + 0x38);
        headers.resize(count);
        ok = count != 0 && count < 512 && readAt(0x20 + entryCount * sizeof(Entry) + tableOffset, headers.data(), count * sizeof(Header));
    }
    HANDLE out = INVALID_HANDLE_VALUE;
    if (ok) {
        // Encrypted or compressed segments cannot be put back.
        for (const auto& entry : entries) {
            if ((entry.props & 0x800) != 0 && (entry.props & 0xA) != 0) ok = false;
        }
        out = ok ? CreateFileW(elfPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
        ok = out != INVALID_HANDLE_VALUE;
    }
    auto writeAt = [&](unsigned long long offset, const void* buffer, DWORD size) {
        LARGE_INTEGER position;
        position.QuadPart = static_cast<LONGLONG>(offset);
        DWORD put = 0;
        return SetFilePointerEx(out, position, nullptr, FILE_BEGIN) && WriteFile(out, buffer, size, &put, nullptr) && put == size;
    };
    if (ok) {
        ok = writeAt(0, elfHead, sizeof(elfHead)) && writeAt(tableOffset, headers.data(), static_cast<DWORD>(headers.size() * sizeof(Header)));
        std::vector<char> chunk(1 << 22);
        unsigned long long end = 0;
        for (const auto& header : headers) end = std::max(end, header.offset + header.fileSize);
        for (const auto& entry : entries) {
            if (!ok || (entry.props & 0x800) == 0) continue;
            const unsigned index = static_cast<unsigned>((entry.props >> 20) & 0xfff);
            if (index >= headers.size()) continue;
            unsigned long long remaining = std::min(entry.fileSize, headers[index].fileSize);
            unsigned long long from = entry.offset, to = headers[index].offset;
            while (ok && remaining != 0) {
                const DWORD part = static_cast<DWORD>(std::min<unsigned long long>(remaining, chunk.size()));
                ok = readAt(from, chunk.data(), part) && writeAt(to, chunk.data(), part);
                from += part;
                to += part;
                remaining -= part;
            }
        }
        if (ok) {
            LARGE_INTEGER size;
            size.QuadPart = static_cast<LONGLONG>(end);
            ok = SetFilePointerEx(out, size, nullptr, FILE_BEGIN) && SetEndOfFile(out);
        }
    }
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    CloseHandle(in);
    if (!ok) DeleteFileW(elfPath.c_str());
    return ok;
}

}

bool ConvertOrCopy(const std::wstring& from, const std::wstring& to) {
    if (IsElf(from)) return CopyFileW(from.c_str(), to.c_str(), FALSE) != 0;
    if (ExtractSelf(from, to)) return true;
    // Dumps re-signed for the console keep the plain ELF beside the SELF, as <name>.esbak.
    const std::wstring backup = from + L".esbak";
    return IsElf(backup) && CopyFileW(backup.c_str(), to.c_str(), FALSE) != 0;
}

void PrepareModules(const std::wstring& game, const std::wstring& prep, std::wstring& failed) {
    CreateDirectoryW(prep.c_str(), nullptr);
    CreateDirectoryW((prep + L"\\sce_module").c_str(), nullptr);
    for (const wchar_t* sub : {L"\\sce_module", L"\\Media\\Modules", L"\\Media\\Plugins"}) {
        WIN32_FIND_DATAW found;
        HANDLE search = FindFirstFileW((game + sub + L"\\*.prx").c_str(), &found);
        if (search == INVALID_HANDLE_VALUE) continue;
        do {
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
            const std::wstring name = found.cFileName;
            if (name.size() < 4 || _wcsicmp(name.c_str() + name.size() - 4, L".prx") != 0) continue;
            const std::wstring target = prep + L"\\sce_module\\" + found.cFileName;
            const std::wstring source = game + sub + L"\\" + found.cFileName;
            if (!ConvertOrCopy(source, target)) failed += (failed.empty() ? L"" : L", ") + std::wstring(found.cFileName);
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
}

std::wstring PickFolder(HWND owner, const std::wstring& start, const wchar_t* title) {
    std::wstring result;
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return result;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(title);
    IShellItem* folder = nullptr;
    if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
        dialog->SetFolder(folder);
        folder->Release();
    }
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(&item))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            result = path;
            CoTaskMemFree(path);
        }
        item->Release();
    }
    dialog->Release();
    while (result.size() > 3 && (result.back() == L'\\' || result.back() == L'/')) result.pop_back();
    return result;
}

namespace {

const unsigned char kSettingsHeader[12] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x00, 0x00};
const char kSettingsBody[] = R"json(ersion":"0.1","globalSaveDataV1ref":{"previouslyBootedGame":true,"previouslySavedDifficultyLevel":0,"previouslyInitializedToDefaults":true,"vsyncOn":false,"isPitUnlocked":false,"previouslySelectedSaveSlotIndex":-1,"masterAudioVolume":0.0,"sfxAudioVolume":0.0,"bgmAudioVolume":0.0,"keybindingsOverride":"","pitData":"","hasBeatenGame":false,"aimAssist":0,"enablePuzzleAimAssist":false,"puzzleTiming":0,"hintsEnabled":true,"hintWaitDuration":1,"disableTutorial":true,"GuidedExperience":true,"GameDifficulty":1,"viewControls":false,"AutoPickupEnabled":false,"menuHolds":0,"repeatedButtonPress":0,"traversalInputs":0,"cinematicSkip":false,"touchpadButton":3,"swipeUp":0,"swipeLeft":0,"swipeRight":0,"swipeDown":0,"controllerVisualization":false,"visualizationContrast":0,"hudCustomization":0,"combatHUD":1,"aimReticle":1,"bossHealthBars":1,"enemyHealthBars":1,"gameplayNotificationToggle":1,"volumeBalance":0,"outputDevice":0,"monoAudioSystemIntegratedSupport":false,"globalVolume":10.0,"dialogueVolume":10.0,"musicVolume":10.0,"sfxVolume":10.0,"controllerSpeakerVolume":10.0,"enableSubtitles":true,"subtitleTextColor":0,"displaySpeakerName":true,"speakerTextColor":0,"enableCaptions":false,"captionTextColor":0,"subtitleAndCaptionTextSize":0,"blurSubtitleAndCaptionBackground":false,"subtitleAndCaptionBackground":1,"audioCue":false,"audioCueVolume":10.0,"audioPanning":0.0,"centerPanDialogue":false,"voiceBoost":false,"uiTextSize":0,"iconSize":0,"highContrastHUD":0,"colorFilter":0,"filterStrength":10.0,"highContrastDisplay":0,"HeroColor":0,"CompanionColor":0,"BossColor":0,"EnemyColor":0,"NPCColor":0,"TargetColor":0,"InteractColor":0,"HazardColor":0,"traversalColor":0,"backgroundColor":0,"enableVsync":true,"screenCalibration":false,"filterMode":0,"enableBloodAndGore":0,"enableBrutalKillPrompt":true,"enableBloodVFX":true,"enableCorpseBloodVFX":true,"fullScreenMode":0,"resolutionIndex":0,"aspectRatio":0,"cameraPanningSpeed":10,"cameraShakeIntensity":10,"activationToggle":false,"horizontalSpeed":0,"verticalSpeed":0,"accelerationSpeed":0,"reduceSmallMotions":0,"textLanguage":0,"speechLanguage":0,"controllerVibration":2,"enableAimToggling":false,"enableBlockToggling":false,"enableHDR":true,"gammaValue":50.5,"brightnessValue":50.5,"personalRecordSpeedrunDataBoy":{"isValid":false,"overallTime":0.0},"chapterBestSpeedrunDataBoy":{"isValid":false,"overallTime":0.0},"personalRecordSpeedrunDataCadet":{"isValid":false,"overallTime":0.0},"chapterBestSpeedrunDataCadet":{"isValid":false,"overallTime":0.0},"personalRecordSpeedrunDataSpartan":{"isValid":false,"overallTime":0.0},"chapterBestSpeedrunDataSpartan":{"isValid":false,"overallTime":0.0},"version":2}})json";
const wchar_t* kSettingsFile = L"\\_sd\\GOWSOSSAVE999\\GoW_SoS999.dat";

// The settings file is kSettingsHeader followed by JSON text with every character stored as four bytes.
std::vector<unsigned char> SeedSettings() {
    std::vector<unsigned char> data(kSettingsHeader, kSettingsHeader + sizeof(kSettingsHeader));
    for (size_t i = 0; kSettingsBody[i] != 0; ++i) {
        data.push_back(static_cast<unsigned char>(kSettingsBody[i]));
        data.insert(data.end(), 3, 0);
    }
    return data;
}

// Before the game has saved its settings once, the seeded defaults stand in for the file.
bool LoadSettings(const std::wstring& workspace, std::vector<unsigned char>& data) {
    const std::wstring path = workspace + kSettingsFile;
    if (!Exists(path)) {
        data = SeedSettings();
        return true;
    }
    const std::string bytes = ReadBytes(path);
    if (bytes.empty()) return false;
    data.assign(bytes.begin(), bytes.end());
    return true;
}

bool StoreSettings(const std::wstring& workspace, const std::vector<unsigned char>& data) {
    CreateDirectoryW((workspace + L"\\_sd").c_str(), nullptr);
    CreateDirectoryW((workspace + L"\\_sd\\GOWSOSSAVE999").c_str(), nullptr);
    const std::wstring path = workspace + kSettingsFile, temporary = path + L".tmp";
    FILE* file = nullptr;
    if (_wfopen_s(&file, temporary.c_str(), L"wb") != 0 || file == nullptr) return false;
    const bool written = fwrite(data.data(), 1, data.size(), file) == data.size();
    if (fclose(file) == 0 && written && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileW(temporary.c_str());
    return false;
}

// Offset of the unsigned integer stored under a key that must occur exactly once, or npos.
size_t FindInteger(const std::vector<unsigned char>& data, const char* key, size_t& digits, int& value) {
    const std::string text = std::string("\"") + key + "\":";
    std::vector<unsigned char> pattern;
    for (const char character : text) {
        pattern.push_back(static_cast<unsigned char>(character));
        pattern.insert(pattern.end(), 3, 0);
    }
    const auto found = std::search(data.begin(), data.end(), pattern.begin(), pattern.end());
    if (found == data.end() || (found - data.begin()) % 4 != 0 || std::search(found + 1, data.end(), pattern.begin(), pattern.end()) != data.end()) return std::string::npos;
    const size_t at = static_cast<size_t>(found - data.begin()) + pattern.size();
    auto unit = [&](size_t index) {
        const size_t offset = at + index * 4;
        return offset + 4 <= data.size() && data[offset + 1] == 0 && data[offset + 2] == 0 && data[offset + 3] == 0 ? data[offset] : 0;
    };
    value = 0;
    for (digits = 0; digits < 9 && unit(digits) >= '0' && unit(digits) <= '9'; ++digits) value = value * 10 + (unit(digits) - '0');
    return digits != 0 && (unit(digits) == ',' || unit(digits) == '}') ? at : std::string::npos;
}

}

int ReadSosSetting(const std::wstring& workspace, const char* key) {
    std::vector<unsigned char> data;
    size_t digits = 0;
    int value = -1;
    return LoadSettings(workspace, data) && FindInteger(data, key, digits, value) != std::string::npos ? value : -1;
}

// Rewrites only the digits of one value, in place and with the same length, so the rest of the file stays byte for byte
// the same. Nothing is written when the value is already set, so a missing file is seeded only when something changes.
bool WriteSosSetting(const std::wstring& workspace, const char* key, int value) {
    std::vector<unsigned char> data;
    size_t digits = 0;
    int current = -1;
    if (!LoadSettings(workspace, data)) return false;
    const size_t at = FindInteger(data, key, digits, current);
    if (at == std::string::npos) return false;
    if (current == value) return true;
    const std::string text = std::to_string(value);
    if (value < 0 || text.size() != digits) return false;
    for (size_t i = 0; i < digits; ++i) data[at + i * 4] = static_cast<unsigned char>(text[i]);
    return StoreSettings(workspace, data);
}

// Once the SDL game window is the active window and presents frames (its title shows the FPS counter), one F11 press and
// release is posted to it, so SDL's fullscreen state stays consistent and F11 keeps working both ways. If the game window
// does not become the active window, nothing is sent, so keys are never sent to another window.
void RequestFullscreen(const PROCESS_INFORMATION& process) {
    for (int i = 0; i < 240 && WaitForSingleObject(process.hProcess, 250) == WAIT_TIMEOUT; ++i) {
        HWND window = GetForegroundWindow();
        DWORD owner = 0;
        wchar_t name[16] = {};
        wchar_t title[256] = {};
        GetWindowThreadProcessId(window, &owner);
        if (owner != process.dwProcessId || GetClassNameW(window, name, 16) == 0 || wcscmp(name, L"SDL_app") != 0) continue;
        if (GetWindowTextW(window, title, 256) == 0 || wcsstr(title, L" | FPS: ") == nullptr) continue;
        const LPARAM scan = static_cast<LPARAM>(MapVirtualKeyW(VK_F11, MAPVK_VK_TO_VSC)) << 16;
        PostMessageW(window, WM_KEYDOWN, VK_F11, scan | 1);
        PostMessageW(window, WM_KEYUP, VK_F11, scan | 1 | 0xC0000000);
        return;
    }
}

}
