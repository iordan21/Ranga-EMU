#include "Runner.hpp"
#include "Dlss5.hpp"
#include "Embed.hpp"
#include "Rooms.hpp"
#include "Systems.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <algorithm>
#include <cctype>
#include <bcrypt.h>
#include <utility>
#include <vector>

namespace Ranga {

Runner::~Runner() {
    if (worker.joinable()) worker.detach();
}

std::wstring Runner::Message() const {
    std::lock_guard guard(lock);
    return message;
}

std::wstring Runner::TitleId() const {
    std::lock_guard guard(lock);
    return titleId;
}

void Runner::Report(State next, const std::wstring& text) {
    {
        std::lock_guard guard(lock);
        message = text;
    }
    state = next;
}

void Runner::Start(const std::wstring& root, const Game& game, const Options& options) {
    if (Busy()) return;
    if (worker.joinable()) worker.join();
    {
        std::lock_guard guard(lock);
        titleId = game.titleId;
    }
    Report(State::Preparing, L"Conferindo os arquivos do jogo...");
    worker = std::thread(&Runner::Run, this, root, game, options);
}

void Runner::StartSystem(const std::wstring& root, const std::wstring& system) {
    Game game;
    game.system = system;
    game.titleId = system + L"-SISTEMA";
    game.title = L"Sistema do " + Wide(SystemName(system));
    game.workspace = WorkspaceFor(root, system, L"Sistema");
    // No folder: RunExternal starts the core with --no-game.
    Start(root, game, Options{});
}

void Runner::Run(std::wstring root, Game game, Options options) {
    const Profile profile = ProfileFor(game.titleId);
    if (profile.bloodborne) {
        RunBloodborne(root, game, options);
        return;
    }
    if (game.system != L"PS5") {
        RunExternal(root, game, options);
        return;
    }
    const std::wstring workspace = game.workspace;
    const std::wstring prep = workspace + L"\\prep";
    const std::wstring logs = workspace + L"\\logs";
    const std::wstring runtime = workspace + L"\\" + kRuntime;
    const std::wstring relinker = root + L"\\tools\\relinker.exe";
    if (!IsGameFolder(game.folder)) {
        Report(State::Failed, L"Os arquivos do jogo não estão mais em " + game.folder + L". Use \"Trocar pasta\" nas opções.");
        return;
    }
    if (!IsDirectory(root + L"\\libs") || !Exists(relinker)) {
        Report(State::Failed, L"Faltam as pastas libs e tools ao lado do RangaEmu.exe. Extraia o download inteiro de novo.");
        return;
    }
    MakeFolders(workspace);
    CreateDirectoryW(prep.c_str(), nullptr);
    CreateDirectoryW(logs.c_str(), nullptr);
    // The runtime finds the system libraries in libs beside itself; every workspace shares the ones beside this program.
    LinkFolder(workspace + L"\\libs", root + L"\\libs", logs + L"\\link.log");

    // What was prepared from another game folder is thrown away, so the game is prepared again from the folder now in use.
    const std::wstring elf = prep + L"\\eboot.elf";
    const std::wstring bin = GameExecutable(game.folder);
    const std::wstring sourceFile = prep + L"\\source.path";
    const std::wstring source = FinalPath(bin);
    const std::wstring sourceOld = ReadPathFile(sourceFile);
    if (!sourceOld.empty() && sourceOld != source) {
        DeleteFileW(elf.c_str());
        DeleteFilesIn(prep + L"\\sce_module");
        SetFileAttributesW(runtime.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(runtime.c_str());
    }
    const FILETIME binTime = WriteTime(bin), elfTime = WriteTime(elf);
    if (!Exists(elf) || CompareFileTime(&elfTime, &binTime) < 0) {
        if (!ConvertOrCopy(bin, elf)) DeleteFileW(elf.c_str());
    }
    if (!Exists(elf) || !IsElf(elf)) {
        Report(State::Failed, L"O executável do jogo não está descriptografado. É preciso um eboot.bin ou eboot.elf descriptografado do seu próprio console.");
        return;
    }
    if (sourceOld != source) WritePathFile(sourceFile, source);

    // The game is prepared again when its executable or the relinker changes.
    const std::wstring stampFile = prep + L"\\prepared.stamp";
    const std::wstring stampNow = L"1 " + std::to_wstring(FileSize(elf)) + L" " + std::to_wstring(FileSize(relinker));
    const std::wstring stampOld = ReadTextLine(stampFile);
    if (Exists(runtime) && stampOld.empty()) WriteTextLine(stampFile, stampNow);
    if (!Exists(runtime) || (!stampOld.empty() && stampOld != stampNow)) {
        Report(State::Preparing, L"Preparando o jogo a partir dos seus arquivos. Só acontece uma vez e leva uns 2 minutos...");
        std::wstring failed;
        PrepareModules(game.folder, prep, failed);
        if (!failed.empty()) {
            Report(State::Failed, L"Alguns módulos do jogo não puderam ser convertidos (" + failed + L"). Eles precisam vir descriptografados do seu console.");
            return;
        }
        const std::wstring registry = workspace + L"\\" + std::wstring(kRuntime, wcslen(kRuntime) - 4) + L".registry.json";
        for (const auto& path : {runtime, registry}) {
            SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(path.c_str());
        }
        std::wstring command = L"\"" + relinker + L"\" --windows --registry --windows-diagnostics --skip-syscall-check ";
        // The relinker refuses a module named here that the game does not have.
        for (const auto& name : profile.excludedModules) {
            if (Exists(prep + L"\\sce_module\\" + name)) command += L"--exclude-sce-module " + name + L" ";
        }
        for (const auto& name : profile.deferredModules) {
            if (Exists(prep + L"\\sce_module\\" + name)) command += L"--defer-sce-module " + name + L" ";
        }
        command += L"\"" + elf + L"\" \"" + runtime + L"\"";
        const long code = RunHidden(command, workspace, logs + L"\\prepare.log");
        if (code != 0 || !Exists(runtime)) {
            Report(State::Failed, L"Não deu para preparar o jogo (código " + std::to_wstring(code) + L"). Veja prepare.log na aba Log.");
            return;
        }
        MoveFileExW((workspace + L"\\windows-diagnostics-imports.txt").c_str(), (logs + L"\\windows-diagnostics-imports.txt").c_str(), MOVEFILE_REPLACE_EXISTING);
        SetFileAttributesW(runtime.c_str(), FILE_ATTRIBUTE_HIDDEN);
        SetFileAttributesW(registry.c_str(), FILE_ATTRIBUTE_HIDDEN);
        WriteTextLine(stampFile, stampNow);
    }

    LinkGameFiles(game.folder, workspace + L"\\app0", logs + L"\\link.log");
    if (profile.sonsOfSparta && options.resolution >= 0) WriteSosSetting(workspace, "resolutionIndex", options.resolution);
    // DLSS 5 comes and goes with its option, so a game started with it off has nothing of it beside the runtime. Its layer
    // variables are set before debug_env.txt is read, which can still override them.
    if (options.dlss5) {
        const std::wstring error = EnableDlss5(root, workspace, workspace, logs + L"\\dlss5.log");
        if (!error.empty()) {
            Report(State::Failed, error);
            return;
        }
    } else {
        DisableDlss5(workspace, workspace);
    }

    // The runtime reads these choices from its environment; an option that is off has no variable, so an inherited value cannot turn it on.
    {
        // A setting one game's profile made must not carry over to the next game started from the same client.
        for (const wchar_t* name : {L"APS5_HOST_IMPORT", L"APS5_HOST_IMPORT_MIB", L"APS5_HOST_POOL_MIB"}) SetEnvironmentVariableW(name, nullptr);
        for (const auto& [name, value] : profile.environment) SetEnvironmentVariableW(name.c_str(), value.c_str());
        FILE* debug = nullptr;
        if (_wfopen_s(&debug, (workspace + L"\\debug_env.txt").c_str(), L"r") == 0 && debug != nullptr) {
            wchar_t entry[512];
            while (fgetws(entry, 512, debug) != nullptr) {
                std::wstring text = entry;
                while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ')) text.pop_back();
                const auto equals = text.find(L'=');
                if (equals != std::wstring::npos && equals != 0 && text[0] != L'#') SetEnvironmentVariableW(text.substr(0, equals).c_str(), text.substr(equals + 1).c_str());
            }
            fclose(debug);
        }
    }
    const bool sos = profile.sonsOfSparta;
    SetEnvironmentVariableW(L"SOS_SKIP_INTRO", sos && options.skipIntro ? L"1" : nullptr);
    SetEnvironmentVariableW(L"SOS_SKIP_DIALOGUE", sos && options.skipDialogue ? L"1" : nullptr);
    SetEnvironmentVariableW(L"SOS_GOD_MODE", sos && options.godMode ? L"1" : nullptr);
    SetEnvironmentVariableW(L"SOS_INFINITE_SPARTAN_SPIRIT", sos && options.infiniteSpirit ? L"1" : nullptr);
    SetEnvironmentVariableW(L"SOS_INFINITE_MAGIC", sos && options.infiniteMagic ? L"1" : nullptr);
    SetEnvironmentVariableW(L"SOS_MOVEMENT_SPEED", sos && options.movementSpeed ? L"1" : nullptr);
    SetEnvironmentVariableW(L"SOS_JUMP_HEIGHT", sos && options.jumpHeight ? L"1" : nullptr);
    SetEnvironmentVariableW(L"SOS_DAMAGE_MULTIPLIER", sos && options.damageMultiplier != 1 ? std::to_wstring(options.damageMultiplier).c_str() : nullptr);

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE out = CreateFileW((logs + L"\\game.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE err = CreateFileW((logs + L"\\game.err").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = out;
    startup.hStdError = err;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    const std::wstring pipelineCache = workspace + L"\\pipeline_cache.bin";
    SetFileAttributesW(pipelineCache.c_str(), FILE_ATTRIBUTE_NORMAL);
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + runtime + L"\"";
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, workspace.c_str(), &startup, &process);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (err != INVALID_HANDLE_VALUE) CloseHandle(err);
    if (!started) {
        Report(State::Failed, L"O jogo não pôde ser iniciado.");
        return;
    }
    AllowSetForegroundWindow(process.dwProcessId);
    Report(State::Running, L"Rodando " + game.title + L"...");
    if (options.borderless) RequestFullscreen(process);
    Await(process, logs, game, L"game.err");
    SetFileAttributesW(pipelineCache.c_str(), FILE_ATTRIBUTE_HIDDEN);
}

void Runner::Await(PROCESS_INFORMATION& process, const std::wstring& logs, const Game& game, const wchar_t* errorLog) {
    const DWORD begun = GetTickCount();
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    const DWORD seconds = (GetTickCount() - begun) / 1000;
    {
        char line[160];
        const int length = std::snprintf(line, sizeof(line), "%s ended, exit code 0x%lx after %lu s\r\n", Utf8(game.titleId).c_str(), code, seconds);
        HANDLE journal = CreateFileW((logs + L"\\launcher.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (journal != INVALID_HANDLE_VALUE) { DWORD put = 0; WriteFile(journal, line, static_cast<DWORD>(length), &put, nullptr); CloseHandle(journal); }
        if (code != 0) CopyFileW((logs + L"\\" + errorLog).c_str(), (logs + L"\\" + errorLog + L".last-error").c_str(), FALSE);
    }
    if (ended) ended(game, static_cast<long long>(std::time(nullptr)) - seconds, seconds, code);
    const bool crashed = (code & 0xf0000000u) == 0xc0000000u && code != 0xcfffffffu;
    wchar_t hex[16];
    std::swprintf(hex, 16, L"0x%lx", code);
    if (crashed) Report(State::Failed, L"O jogo parou de repente (código " + std::wstring(hex) + L") depois de " + std::to_wstring(seconds) + L" s. Veja " + errorLog + L" na aba Log.");
    else if (code != 0 && seconds < 90 && ProfileFor(game.titleId).bloodborne) Report(State::Failed, L"O port do Bloodborne parou ao preparar o jogo (código " + std::to_wstring(code) + L"). Veja game.log na aba Log.");
    else Report(State::Finished, L"Jogo fechado depois de " + std::to_wstring(seconds / 60) + L" min " + std::to_wstring(seconds % 60) + L" s.");
}

std::string PlayerRpcnName(const std::wstring& root) {
    const std::wstring ini = root + L"\\engines.ini";
    std::wstring wanted = ReadIni(ini, L"Ranga", L"online_nome", L"");
    if (wanted.empty()) wanted = ReadIni(ini, L"SWITCH", L"jogador", L"");
    const auto keep = [](const std::wstring& from) {
        std::string name;
        for (const wchar_t c : from) {
            if ((c < 128 && std::isalnum(static_cast<int>(c))) || c == L'-' || c == L'_') name += static_cast<char>(c);
            if (name.size() == 16) break;
        }
        return name;
    };
    std::string name = keep(wanted);
    if (name.size() < 3) {
        wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
        GetComputerNameW(computer, &size);
        name = keep(computer);
    }
    if (name.size() < 3) name = "JogadorRanga";
    return name;
}

namespace {

// Turns the PS3 core's network on (internet, RPCN, UPnP for the games' direct links) or off, in its config.yml's Net
// section; the other lines stay as they are. Nothing to do while the file doesn't exist (the core makes it on its first
// start, network off, which is RPCS3's default).
void SetPs3Network(const std::wstring& root, bool on) {
    const std::wstring path = root + L"\\bios\\retroarch\\rpcs3\\config\\config.yml";
    const std::string text = ReadBytes(path);
    if (text.empty()) return;
    std::string out;
    bool inNet = false, changed = false;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        const bool last = end == std::string::npos;
        if (last) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        const bool cr = !line.empty() && line.back() == '\r';
        if (cr) line.pop_back();
        if (!line.empty() && line[0] != ' ') inNet = line == "Net:";
        if (inNet) {
            const auto set = [&](const char* key, const char* value) {
                const std::string prefix = std::string("  ") + key + ":";
                if (line.rfind(prefix, 0) != 0) return;
                const std::string wanted = prefix + " " + value;
                if (line != wanted) line = wanted, changed = true;
            };
            set("Internet enabled", on ? "Connected" : "Disconnected");
            set("PSN status", on ? "RPCN" : "Disconnected");
            set("UPNP Enabled", on ? "true" : "false");
        }
        out += line;
        if (cr) out += '\r';
        if (!last) out += '\n';
    }
    if (!changed) return;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(file, out.data(), static_cast<DWORD>(out.size()), &put, nullptr);
    CloseHandle(file);
}

// This PC's password on Ranga's RPCN servers: made once (24 random hex digits) and kept in engines.ini; nobody types it.
std::string RpcnPassword(const std::wstring& root) {
    const std::wstring ini = root + L"\\engines.ini";
    std::string password = Utf8(ReadIni(ini, L"Ranga", L"rpcn_senha", L""));
    if (password.size() >= 16) return password;
    unsigned char bytes[12] = {};
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return "";
    static const char hex[] = "0123456789abcdef";
    password.clear();
    for (const unsigned char b : bytes) password += hex[b >> 4], password += hex[b & 15];
    EnsureUnicodeIni(ini);
    WritePrivateProfileStringW(L"Ranga", L"rpcn_senha", Wide(password).c_str(), ini.c_str());
    return password;
}

// The PS3 core's account (rpcn.yml, beside its config.yml): on Ranga's server, this player's name and this PC's
// password at the address the game asked for (the core makes the account there on first use); on the official RPCN,
// the account made in RPCS3's own window, copied over when there is one.
void WriteRpcnAccount(const std::wstring& root, const Game& game) {
    const std::wstring folder = root + L"\\bios\\retroarch\\rpcs3\\config";
    CreateDirectoryW(folder.c_str(), nullptr);
    if (ReadCoreOption(game, "rpcs3_servidor") == "oficial") {
        const std::wstring standalone = root + L"\\emuladores\\rpcs3\\config\\rpcn.yml";
        if (Exists(standalone)) CopyFileW(standalone.c_str(), (folder + L"\\rpcn.yml").c_str(), FALSE);
        return;
    }
    std::string host = ReadCoreOption(game, "rpcs3_servidor_endereco");
    host.erase(std::remove_if(host.begin(), host.end(), [](char c) { return c == '"' || c == ' ' || c == '\r' || c == '\n'; }), host.end());
    const std::string password = RpcnPassword(root);
    if (host.empty() || password.empty()) return;
    if (host.find(':') == std::string::npos) host += ":31313";
    const std::string text = "Version: 2\nHost: \"" + host + "\"\nNPID: " + PlayerRpcnName(root) + "\nPassword: " + password +
                             "\nToken: \"\"\nHosts: \"Ranga|" + host + "\"\nExperimental IPv6 support: false\n";
    const HANDLE file = CreateFileW((folder + L"\\rpcn.yml").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &put, nullptr);
    CloseHandle(file);
}

// Python 3.10 or newer, which the port's scripts need. Each candidate is asked for its version: the Microsoft Store's
// python.exe in WindowsApps is a real Python once installed from the Store, and only a stub that opens the Store before.
std::wstring FindPython(const std::wstring& logs) {
    std::vector<std::wstring> candidates;
    wchar_t local[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    for (int minor = 20; minor >= 10; --minor) {
        for (const std::wstring base : {std::wstring(L"C:\\Program Files\\Python3"), std::wstring(local) + L"\\Programs\\Python\\Python3"}) {
            const std::wstring path = base + std::to_wstring(minor) + L"\\python.exe";
            if (Exists(path)) candidates.push_back(path);
        }
    }
    wchar_t found[MAX_PATH * 2] = {};
    for (const wchar_t* name : {L"python.exe", L"py.exe"}) {
        if (SearchPathW(nullptr, name, nullptr, MAX_PATH * 2, found, nullptr) != 0) candidates.push_back(found);
    }
    for (const auto& path : candidates) {
        const std::wstring check = L"\"" + path + L"\" -c \"import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)\"";
        if (RunHidden(check, logs, logs + L"\\python-check.log") == 0) return path;
    }
    return L"";
}

}

void Runner::RunBloodborne(const std::wstring& root, const Game& game, const Options& options) {
    const std::wstring engine = BloodborneEngine(root);
    const std::wstring workspace = game.workspace;
    const std::wstring logs = workspace + L"\\logs";
    if (!Exists(game.folder + L"\\eboot.bin")) {
        Report(State::Failed, L"Não há eboot.bin em " + game.folder + L". Use \"Trocar pasta\" nas opções.");
        return;
    }
    if (!Exists(engine + L"\\run.bat") || !Exists(engine + L"\\out\\bbport.exe")) {
        Report(State::Failed, L"Falta o port do Bloodborne em " + engine + L" (run.bat e out\\bbport.exe).");
        return;
    }
    MakeFolders(workspace);
    CreateDirectoryW(logs.c_str(), nullptr);
    const std::wstring python = FindPython(logs);
    if (python.empty()) {
        Report(State::Failed, L"O port do Bloodborne precisa do Python 3.10 ou mais novo (python.org ou Microsoft Store). Instale e tente de novo.");
        return;
    }
    // Its saves, settings and the files it makes from the eboot live in the workspace; the port's own folder is only read.
    const std::wstring config = workspace + L"\\bbport.ini";
    if (!Exists(config)) CopyFileW((engine + L"\\bbport.ini").c_str(), config.c_str(), TRUE);
    // ReShade and the add-ons have to sit beside bbport.exe, the one place this goes into the port's folder; what goes in
    // is listed in the workspace and taken out when the option is off.
    if (options.dlss5) {
        const std::wstring error = EnableDlss5(root, engine + L"\\out", workspace, logs + L"\\dlss5.log");
        if (!error.empty()) {
            Report(State::Failed, error);
            return;
        }
    } else {
        DisableDlss5(engine + L"\\out", workspace);
    }

    const std::vector<std::pair<const wchar_t*, std::wstring>> environment = {
        {L"PYTHON", python},
        {L"BB_DATA_DIR", workspace},
        {L"BB_CONFIG", config},
        {L"BB_GAME_DIR", game.folder},
        {L"BB_FPS", options.fps},
        {L"BB_RENDER_RES", options.renderResolution},
        {L"BB_FULLSCREEN", options.borderless ? L"1" : L"0"},
        {L"BB_FRAMES_AHEAD", L"2"},
        {L"BB_MODS_ENABLED", L"0"},
        {L"BB_PATCHES", options.skipIntro ? L"Skip Intro" : L""},
    };
    for (const auto& [name, value] : environment) SetEnvironmentVariableW(name, value.empty() ? nullptr : value.c_str());

    // The port's scripts and bbport.exe write progress and errors to both streams; one log keeps them in order.
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE out = CreateFileW((logs + L"\\game.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DeleteFileW((logs + L"\\game.err").c_str());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = out;
    startup.hStdError = out;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    wchar_t system[MAX_PATH] = {};
    GetSystemDirectoryW(system, MAX_PATH);
    // run.bat works from its own folder; the game folder goes as its argument too, as its own launcher passes it.
    std::wstring command = L"\"" + std::wstring(system) + L"\\cmd.exe\" /d /c \"\"" + engine + L"\\run.bat\" \"" + game.folder + L"\"\"";
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, engine.c_str(), &startup, &process);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!started) {
        Report(State::Failed, L"O port do Bloodborne não pôde ser iniciado.");
        return;
    }
    AllowSetForegroundWindow(ASFW_ANY);
    Report(State::Running, L"Rodando " + game.title + L" pelo port nativo...");
    Await(process, logs, game, L"game.log");
}

void Runner::RunExternal(const std::wstring& root, const Game& game, const Options& options) {
    const Engine engine = LoadEngine(root, game.system);
    const std::wstring workspace = game.workspace;
    const std::wstring logs = workspace + L"\\logs";
    if (!engine.ready) {
        Report(State::Failed, engine.problem);
        return;
    }
    // The console's system (StartSystem) has no game: Ranga Retro starts the core with --no-game.
    const bool noGame = game.folder.empty() && engine.native;
    const std::wstring rom = noGame ? L"" : GameRom(game.system, game.folder);
    if (rom.empty() && !noGame) {
        Report(State::Failed, L"O jogo não está mais em " + game.folder + L". Use \"Trocar\" nas opções.");
        return;
    }
    // Named here instead of the emulator opening to an error or a black screen.
    if (const std::wstring missing = MissingFiles(root, game.system, engine, rom); !missing.empty()) {
        Report(State::Failed, missing);
        return;
    }
    MakeFolders(workspace);
    CreateDirectoryW(logs.c_str(), nullptr);
    // As with the Bloodborne port, ReShade and the add-ons sit beside the emulator only while this game has DLSS 5 on.
    // The layer reaches only Vulkan emulators; one drawing with Direct3D or OpenGL just starts without it.
    const std::wstring exeFolder = ParentFolder(engine.exe);
    // Ranga Retro presents through Vulkan when its core draws with Vulkan (PS1, N64 with paraLLEl-RDP), so the layer
    // reaches those native games too; a core drawing in software or OpenGL starts without it.
    if (options.dlss5) {
        const std::wstring error = EnableDlss5(root, exeFolder, workspace, logs + L"\\dlss5.log");
        if (!error.empty()) {
            Report(State::Failed, error);
            return;
        }
    } else {
        DisableDlss5(exeFolder, workspace);
    }

    // An embedded emulator stays windowed inside Ranga's window, which goes fullscreen instead.
    CreateDirectoryW((workspace + L"\\saves").c_str(), nullptr);
    // A PSP homebrew kept as on the memory stick (...\PSP\GAME\<name>\EBOOT.PBP): PPSSPP's core sees "/PSP/GAME/" in
    // the path and looks for it on its own memory stick (ms0:), where it isn't, then fails (20/11/2026, OpenHCL). The
    // core gets the same folder through a link in the workspace, whose path doesn't say PSP\GAME.
    std::wstring coreRom = rom;
    if (engine.native && game.system == L"PSP" && !rom.empty()) {
        std::wstring upper = rom;
        for (auto& character : upper) character = static_cast<wchar_t>(towupper(character == L'/' ? L'\\' : character));
        const size_t slash = rom.find_last_of(L"\\/");
        if (upper.find(L"\\PSP\\GAME\\") != std::wstring::npos && slash != std::wstring::npos) {
            const std::wstring link = workspace + L"\\pasta-do-jogo";
            LinkFolder(link, rom.substr(0, slash), logs + L"\\pasta-do-jogo.log");
            if (FinalPath(link) == FinalPath(rom.substr(0, slash))) coreRom = link + rom.substr(slash);
        }
    }
    // The PS3's network is in the core's own config.yml (Net), read when the game boots: on for a game with online
    // (RPCN) chosen, off otherwise, so a game without it never tries to log in.
    if (engine.native && game.system == L"PS3") {
        const bool online = ReadCoreOption(game, "rpcs3_online") == "ligado";
        SetPs3Network(root, online);
        if (online) WriteRpcnAccount(root, game);
    }
    std::wstring command = EngineCommand(engine, coreRom, options.borderless && !engine.embed, game.title, workspace);
    if (noGame) {
        if (const size_t at = command.find(L"--rom \"\""); at != std::wstring::npos) command.replace(at, 8, L"--no-game");
    }
    // The PC that hosts the Switch room joins it right here, not through the tunnel: through playit its own game's
    // packets went out to playit's server and back, which adds delay and one more thing that can drop (20/11/2026 the
    // tunnel didn't answer in time and Mario Kart lost the friend). Only for this run; the game keeps its address.
    if (engine.native && game.system == L"SWITCH" && ReadCoreOption(game, "eden_sala") == "entrar") {
        std::string address = ReadCoreOption(game, "eden_sala_endereco");
        // The Porta field holds the room's own port (24872); the one in a tunnel address is playit's, which no room has.
        const int port = std::atoi(ReadCoreOption(game, "eden_sala_porta").c_str());
        if (const size_t colon = address.rfind(':'); colon != std::string::npos) address.resize(colon);
        if (!address.empty() && address != "127.0.0.1" && address != "localhost" && RoomServerEnabled(root)) {
            // The room this game asked for by its port, else the only one running here.
            std::vector<Room> running;
            for (const Room& room : LoadRooms(root)) {
                if (RoomRunning(root, room)) running.push_back(room);
            }
            const Room* local = nullptr;
            for (const Room& room : running) {
                if (room.port == port) local = &room;
            }
            if (local == nullptr && running.size() == 1) local = &running.front();
            if (local != nullptr) command += L" --option \"eden_sala_endereco=127.0.0.1:" + std::to_wstring(local->port) + L"\"";
        }
    }
    // The PC that runs the Xbox hub reaches it here too, not through its own tunnel.
    if (engine.native && game.system == L"XBOX" && ReadCoreOption(game, "xemu_network_backend") == "udp") {
        const std::string hub = ReadCoreOption(game, "xemu_network_udp_remote");
        if (!hub.empty() && hub.rfind("127.0.0.1", 0) != 0 && RoomServerEnabled(root) && ExtraRunning(root, Extra::Classics))
            command += L" --option \"xemu_network_udp_remote=127.0.0.1:" + std::to_wstring(XboxHubPort(root)) + L"\"";
    }
    {
        // The exact command goes to the log first, so a wrong argument shows there.
        const std::string line = "> " + Utf8(command) + "\r\n";
        HANDLE journal = CreateFileW((logs + L"\\launcher.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (journal != INVALID_HANDLE_VALUE) { DWORD put = 0; WriteFile(journal, line.data(), static_cast<DWORD>(line.size()), &put, nullptr); CloseHandle(journal); }
    }
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE out = CreateFileW((logs + L"\\game.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DeleteFileW((logs + L"\\game.err").c_str());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = out;
    startup.hStdError = out;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    // Emulators keep their settings, BIOS and saves beside themselves when portable, so they start from their own folder.
    // Ranga Retro tells the console by it, for RetroAchievements (retro/Achievements.hpp).
    SetEnvironmentVariableW(L"RANGA_SISTEMA", game.system.c_str());
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, exeFolder.c_str(), &startup, &process);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!started) {
        Report(State::Failed, L"O emulador não pôde ser iniciado (" + engine.exe + L").");
        return;
    }
    AllowSetForegroundWindow(ASFW_ANY);
    const std::wstring emulator = engine.presetName.empty() ? FileName(engine.exe) : engine.presetName;
    Report(State::Running, L"Rodando " + game.title + L" no " + emulator + L"...");
    if (engine.embed) EmbedUntilExit(process, game.title, options.borderless);
    Await(process, logs, game, L"game.log");
}

}
