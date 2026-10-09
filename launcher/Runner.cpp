#include "Runner.hpp"

#include <cstdio>
#include <cwchar>

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

void Runner::Run(std::wstring root, Game game, Options options) {
    const Profile profile = ProfileFor(game.titleId);
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
    CreateDirectoryW((root + L"\\games").c_str(), nullptr);
    CreateDirectoryW(workspace.c_str(), nullptr);
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
    const DWORD begun = GetTickCount();
    if (options.borderless) RequestFullscreen(process);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    SetFileAttributesW(pipelineCache.c_str(), FILE_ATTRIBUTE_HIDDEN);
    const DWORD seconds = (GetTickCount() - begun) / 1000;
    {
        char line[160];
        const int length = std::snprintf(line, sizeof(line), "ended, exit code 0x%lx after %lu s\r\n", code, seconds);
        HANDLE journal = CreateFileW((logs + L"\\launcher.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (journal != INVALID_HANDLE_VALUE) { DWORD put = 0; WriteFile(journal, line, static_cast<DWORD>(length), &put, nullptr); CloseHandle(journal); }
        if (code != 0) CopyFileW((logs + L"\\game.err").c_str(), (logs + L"\\game.err.last-error").c_str(), FALSE);
    }
    const bool crashed = (code & 0xf0000000u) == 0xc0000000u && code != 0xcfffffffu;
    wchar_t hex[16];
    std::swprintf(hex, 16, L"0x%lx", code);
    if (crashed) Report(State::Failed, L"O jogo parou de repente (código " + std::wstring(hex) + L") depois de " + std::to_wstring(seconds) + L" s. Veja game.err na aba Log.");
    else Report(State::Finished, L"Jogo fechado depois de " + std::to_wstring(seconds / 60) + L" min " + std::to_wstring(seconds % 60) + L" s.");
}

}
