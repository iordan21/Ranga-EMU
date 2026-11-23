#include "Install.hpp"
#include "Common.hpp"
#include "Idioma.hpp"

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <string>

namespace Ranga {

namespace {

constexpr const wchar_t* kDefaultFolder = L"C:\\Jogos\\Ranga";

std::wstring Lowered(std::wstring text) {
    for (auto& c : text) c = static_cast<wchar_t>(towlower(c));
    return text;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    wchar_t* path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) result = path;
    CoTaskMemFree(path);
    return result;
}

bool Inside(const std::wstring& folder, const std::wstring& parent) {
    if (parent.empty()) return false;
    const std::wstring a = Lowered(folder) + L"\\", b = Lowered(parent) + L"\\";
    return a.rfind(b, 0) == 0;
}

// Where a Ranga should not live: the Desktop and Downloads (often OneDrive, and the emulators break on the accent in
// "Área de Trabalho"), Temp (an exe opened from inside a zip runs from there and is gone after), or any path with
// letters outside ASCII.
bool BadPlace(const std::wstring& folder) {
    for (wchar_t c : folder)
        if (c > 127) return true;
    const std::wstring low = Lowered(folder);
    if (low.find(L"onedrive") != std::wstring::npos) return true;
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    std::wstring tempFolder = temp;
    while (!tempFolder.empty() && tempFolder.back() == L'\\') tempFolder.pop_back();
    return Inside(folder, tempFolder) || Inside(folder, KnownFolder(FOLDERID_Desktop)) || Inside(folder, KnownFolder(FOLDERID_Downloads));
}

bool MakeFolders(const std::wstring& folder) {
    const int result = SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS || result == ERROR_FILE_EXISTS;
}

void Shortcut(const std::wstring& lnk, const std::wstring& folder) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&link)))) return;
    link->SetPath((folder + L"\\RangaEmu.exe").c_str());
    link->SetWorkingDirectory(folder.c_str());
    link->SetDescription(L"Ranga Emu");
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))) {
        file->Save(lnk.c_str(), TRUE);
        file->Release();
    }
    link->Release();
}

bool Open(const std::wstring& exe) {
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    // --instalado: the copy is the Ranga, never asked again (a flag skips OfferInstall).
    std::wstring command = L"\"" + exe + L"\" --instalado";
    std::wstring folder = exe.substr(0, exe.find_last_of(L'\\'));
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &startup, &process)) return false;
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return true;
}

std::wstring PickFolder() {
    IFileOpenDialog* dialog = nullptr;
    std::wstring chosen;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return chosen;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(Idioma::TrW(L"Onde instalar o Ranga").c_str());
    if (SUCCEEDED(dialog->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            wchar_t* path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) chosen = path;
            CoTaskMemFree(path);
            item->Release();
        }
    }
    dialog->Release();
    if (chosen.empty()) return chosen;
    // A folder named Ranga is the place itself; anything else gets a Ranga folder inside.
    const size_t slash = chosen.find_last_of(L'\\');
    const std::wstring name = Lowered(slash == std::wstring::npos ? chosen : chosen.substr(slash + 1));
    if (name != L"ranga") chosen += chosen.back() == L'\\' ? L"Ranga" : L"\\Ranga";
    return chosen;
}

void Error(const std::wstring& text) { MessageBoxW(nullptr, text.c_str(), L"Ranga Emu", MB_OK | MB_ICONWARNING); }

// Copies this exe to folder, makes the shortcuts and opens the copy. False with a message when it could not.
bool InstallTo(const std::wstring& self, const std::wstring& folder) {
    if (!MakeFolders(folder)) {
        Error(Idioma::TrW(L"Não deu para criar a pasta") + L" " + folder + L".");
        return false;
    }
    const std::wstring target = folder + L"\\RangaEmu.exe";
    if (Lowered(target) != Lowered(self) && !CopyFileW(self.c_str(), target.c_str(), FALSE)) {
        Error(Idioma::TrW(L"Não deu para copiar o Ranga para") + L" " + folder + L".");
        return false;
    }
    // RANGA_INSTALAR_ATALHOS (tests): both shortcuts go to that folder instead of the Desktop and the Start menu.
    const wchar_t* testLinks = _wgetenv(L"RANGA_INSTALAR_ATALHOS");
    const std::wstring desktop = testLinks != nullptr ? std::wstring(testLinks) : KnownFolder(FOLDERID_Desktop);
    const std::wstring programs = testLinks != nullptr ? std::wstring(testLinks) + L"\\menu" : KnownFolder(FOLDERID_Programs);
    if (testLinks != nullptr) MakeFolders(programs);
    if (!desktop.empty()) Shortcut(desktop + L"\\Ranga Emu.lnk", folder);
    if (!programs.empty()) Shortcut(programs + L"\\Ranga Emu.lnk", folder);
    if (!Open(target)) {
        Error(Idioma::TrW(L"O Ranga foi instalado, mas não abriu sozinho. Abra pelo atalho Ranga Emu na Área de Trabalho."));
    }
    return true;
}

}

bool FirstInstall(const std::wstring& root) { return !Exists(root + L"\\ranga_retro.exe"); }

bool OfferInstall(const std::wstring& root) {
    // Ranga has run here already (it makes jogos on its first start), or this is a whole Ranga folder.
    if (Exists(root + L"\\engines.ini") || Exists(root + L"\\library.ini") || Exists(root + L"\\cores") || Exists(root + L"\\jogos")) return false;
    wchar_t selfPath[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, selfPath, MAX_PATH * 2);
    const std::wstring self = selfPath;

    if (const wchar_t* test = _wgetenv(L"RANGA_INSTALAR_PASTA"); test != nullptr && *test != 0) return InstallTo(self, test);
    if (_wgetenv(L"RANGA_CLIENT_TEST_INPUT") != nullptr) return false;

    Idioma::Init(root);
    const bool installed = Exists(std::wstring(kDefaultFolder) + L"\\RangaEmu.exe") && Lowered(root) != Lowered(kDefaultFolder);
    const bool badHere = BadPlace(root);

    enum { kInstall = 100, kPick, kHere, kOpenInstalled };
    const std::wstring installLabel = installed ? Idioma::TrW(L"Abrir o Ranga já instalado") + L"\n" + kDefaultFolder
                                                : Idioma::TrW(L"Instalar em") + L" " + kDefaultFolder + L"\n" + Idioma::TrW(L"Recomendado: cria a pasta e os atalhos");
    const std::wstring pickLabel = Idioma::TrW(L"Instalar em outra pasta...");
    const std::wstring hereLabel = Idioma::TrW(L"Usar esta pasta mesmo") + L"\n" + root;
    TASKDIALOG_BUTTON buttons[3];
    UINT count = 0;
    buttons[count++] = {installed ? static_cast<int>(kOpenInstalled) : static_cast<int>(kInstall), installLabel.c_str()};
    buttons[count++] = {kPick, pickLabel.c_str()};
    if (!badHere) buttons[count++] = {kHere, hereLabel.c_str()};

    const std::wstring title = Idioma::TrW(L"Instalar o Ranga Emu");
    const std::wstring heading = installed ? Idioma::TrW(L"O Ranga já está instalado neste PC.") : Idioma::TrW(L"Onde o Ranga vai ficar?");
    std::wstring body = Idioma::TrW(L"O Ranga precisa de uma pasta só dele. Na primeira vez que abrir, ele baixa sozinho o resto dos arquivos (uns 450 MB).");
    if (badHere) body += L"\n\n" + Idioma::TrW(L"Esta pasta não serve (Área de Trabalho, Downloads, OneDrive, dentro de um zip ou com acento no caminho): os emuladores dão erro nela.");
    TASKDIALOGCONFIG config{sizeof(config)};
    config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    config.pszWindowTitle = title.c_str();
    config.pszMainInstruction = heading.c_str();
    config.pszContent = body.c_str();
    config.pButtons = buttons;
    config.cButtons = count;
    config.nDefaultButton = buttons[0].nButtonID;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.hInstance = GetModuleHandleW(nullptr);
    config.dwFlags |= TDF_USE_HICON_MAIN;
    config.hMainIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));

    for (;;) {
        int pressed = 0;
        if (FAILED(TaskDialogIndirect(&config, &pressed, nullptr, nullptr))) return false;
        switch (pressed) {
        case kInstall:
            if (InstallTo(self, kDefaultFolder)) return true;
            break;
        case kOpenInstalled:
            if (Open(std::wstring(kDefaultFolder) + L"\\RangaEmu.exe")) return true;
            break;
        case kPick: {
            const std::wstring folder = PickFolder();
            if (folder.empty()) break;
            if (BadPlace(folder)) {
                Error(Idioma::TrW(L"Essa pasta não serve (Área de Trabalho, Downloads, OneDrive, dentro de um zip ou com acento no caminho). Escolha outra, como C:\\Jogos."));
                break;
            }
            if (InstallTo(self, folder)) return true;
            break;
        }
        case kHere:
            return false;
        default:
            return true; // Cancel: nothing installed, nothing opened.
        }
    }
}

}
