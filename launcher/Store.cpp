#include "Store.hpp"
#include "Common.hpp"
#include "Update.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>

namespace Ranga {

namespace {

constexpr const wchar_t* kDefaultStore = L"https://github.com/iordan21/Ranga-Apps/releases/latest/download/catalogo.txt";

std::string Trim(std::string text) {
    while (!text.empty() && (text.back() == '\r' || text.back() == ' ' || text.back() == '\n')) text.pop_back();
    size_t start = 0;
    while (start < text.size() && text[start] == ' ') ++start;
    return text.substr(start);
}

bool SafeId(const std::string& id) {
    if (id.empty() || id.size() > 32) return false;
    return std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

// The program inside the zip: a plain name ending in .exe, never a path.
bool SafeExe(const std::wstring& exe) {
    if (exe.size() < 5 || exe.find_first_of(L"\\/:\"") != std::wstring::npos || exe.find(L"..") != std::wstring::npos) return false;
    return Lower(exe.substr(exe.size() - 4)) == L".exe";
}

// Arguments the app gets: words of letters, digits, '-', '=', '.' and ',' (no quotes, nothing for the shell).
bool SafeArgs(const std::wstring& args) {
    return std::all_of(args.begin(), args.end(), [](wchar_t c) {
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'=' || c == L'.' || c == L',' || c == L' ';
    });
}

// Downloads only from the owner's GitHub (a release asset there), or from this PC (a local test).
bool SafeZip(const std::wstring& url) {
    return url.rfind(L"https://github.com/iordan21/", 0) == 0 || url.rfind(L"http://127.0.0.1:", 0) == 0;
}

std::wstring Marker(const std::wstring& folder) { return folder + L"\\ranga-app.txt"; }

}

std::vector<StoreApp> ReadCatalog(const std::string& text) {
    std::vector<StoreApp> apps;
    StoreApp current;
    bool open = false;
    const auto close = [&] {
        if (open && SafeId(current.id) && !current.name.empty() && current.sha256.size() == 64 && current.size > 0 && SafeZip(current.zip) &&
            SafeExe(current.exe) && SafeArgs(current.args) && SafeArgs(current.tvArgs) &&
            std::none_of(apps.begin(), apps.end(), [&](const StoreApp& app) { return app.id == current.id; }))
            apps.push_back(current);
        current = StoreApp{};
        open = false;
    };
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = Trim(text.substr(start, end - start));
        start = end + 1;
        const size_t space = line.find(' ');
        const std::string key = line.substr(0, space), value = space == std::string::npos ? "" : Trim(line.substr(space + 1));
        if (key == "app") {
            close();
            current.id = value;
            open = true;
        } else if (!open) {
            continue;
        } else if (key == "nome") current.name = value;
        else if (key == "versao") current.version = value;
        else if (key == "descricao") current.description = value;
        else if (key == "zip") current.zip = Wide(value);
        else if (key == "sha256") current.sha256 = value;
        else if (key == "tamanho") current.size = std::strtoull(value.c_str(), nullptr, 10);
        else if (key == "exe") current.exe = Wide(value);
        else if (key == "args") current.args = Wide(value);
        else if (key == "args_tv") current.tvArgs = Wide(value);
    }
    close();
    return apps;
}

std::wstring StoreAddress(const std::wstring& root) {
    const std::wstring chosen = ReadIni(root + L"\\engines.ini", L"Ranga", L"loja", L"");
    return chosen.empty() ? kDefaultStore : chosen;
}

std::wstring StoreAppFolder(const std::wstring& root, const std::string& id) { return root + L"\\apps\\" + Wide(id); }

std::wstring StoreAppExe(const std::wstring& root, const StoreApp& app) { return StoreAppFolder(root, app.id) + L"\\" + app.exe; }

std::vector<StoreApp> InstalledApps(const std::wstring& root) {
    std::vector<StoreApp> apps;
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((root + L"\\apps\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return apps;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || found.cFileName[0] == L'.') continue;
        const std::wstring folder = root + L"\\apps\\" + found.cFileName;
        for (StoreApp app : ReadCatalog(ReadBytes(Marker(folder)))) {
            if (Wide(app.id) != found.cFileName || !Exists(folder + L"\\" + app.exe)) continue;
            app.installed = true;
            app.installedVersion = app.version;
            apps.push_back(app);
        }
    } while (FindNextFileW(search, &found));
    FindClose(search);
    return apps;
}

StoreCatalog FetchStore(const std::wstring& root) {
    StoreCatalog catalog;
    const std::vector<StoreApp> installed = InstalledApps(root);
    const std::wstring address = StoreAddress(root);
    std::string text, signature;
    if (DownloadUrl(address, &text, L"", nullptr) != 200 || DownloadUrl(address.substr(0, address.size() - 4) + L".sig", &signature, L"", nullptr) != 200) {
        catalog.error = L"A lista de apps não veio (sem internet, ou ainda não foi publicada).";
    } else if (!SignedByRanga(text, signature)) {
        catalog.error = L"A lista de apps não tem a assinatura do Ranga: ignorada.";
    } else {
        catalog.apps = ReadCatalog(text);
    }
    // What is installed: marked on the catalog's entry, or shown on its own (gone from the catalog, or offline).
    for (const StoreApp& mine : installed) {
        auto known = std::find_if(catalog.apps.begin(), catalog.apps.end(), [&](const StoreApp& app) { return app.id == mine.id; });
        if (known == catalog.apps.end()) {
            catalog.apps.push_back(mine);
            continue;
        }
        known->installed = true;
        known->installedVersion = mine.installedVersion;
        known->newer = known->sha256 != mine.sha256;
    }
    return catalog;
}

std::wstring InstallStoreApp(const std::wstring& root, const StoreApp& app, const std::function<void(uint64_t, uint64_t)>& progress) {
    if (!SafeId(app.id) || !SafeZip(app.zip) || !SafeExe(app.exe)) return L"Esse app não veio certo na lista.";
    const std::wstring apps = root + L"\\apps", folder = StoreAppFolder(root, app.id);
    const std::wstring zip = apps + L"\\" + Wide(app.id) + L".baixando.zip", fresh = folder + L".nova", old = folder + L".antiga";
    CreateDirectoryW(apps.c_str(), nullptr);
    DeleteFileW(zip.c_str());
    const int status = DownloadUrl(app.zip, nullptr, zip, [&](uint64_t got) {
        if (progress) progress(got, app.size);
    });
    if (status != 200) {
        DeleteFileW(zip.c_str());
        return L"O download de " + Wide(app.name) + L" parou (sem internet?).";
    }
    if (Sha256OfFile(zip) != app.sha256) {
        DeleteFileW(zip.c_str());
        return Wide(app.name) + L" chegou diferente do que foi publicado: não foi instalado.";
    }
    RemoveFolderTree(fresh);
    RemoveFolderTree(old);
    CreateDirectoryW(fresh.c_str(), nullptr);
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    std::string said;
    const int code = RunHiddenCommand(L"\"" + std::wstring(system) + L"\\tar.exe\" -x -f \"" + zip + L"\" -C \"" + fresh + L"\"", fresh, said);
    DeleteFileW(zip.c_str());
    if (code != 0 || !Exists(fresh + L"\\" + app.exe)) {
        RemoveFolderTree(fresh);
        if (said.size() > 200) said = said.substr(said.size() - 200);
        return L"Não deu para abrir o zip de " + Wide(app.name) + L" (código " + std::to_wstring(code) + L"): " + Wide(said);
    }
    // Its catalog entry beside it: opening it later needs no internet.
    const std::string entry = "ranga-apps 1\napp " + app.id + "\nnome " + app.name + "\nversao " + app.version + "\ndescricao " + app.description + "\nzip " +
                              Utf8(app.zip) + "\nsha256 " + app.sha256 + "\ntamanho " + std::to_string(app.size) + "\nexe " + Utf8(app.exe) + "\nargs " +
                              Utf8(app.args) + "\nargs_tv " + Utf8(app.tvArgs) + "\n";
    if (FILE* file = _wfopen(Marker(fresh).c_str(), L"wb")) {
        std::fwrite(entry.data(), 1, entry.size(), file);
        std::fclose(file);
    }
    // A folder whose program is open cannot be renamed: an open app is noticed before anything is touched.
    if (IsDirectory(folder) && !MoveFileExW(folder.c_str(), old.c_str(), 0)) {
        RemoveFolderTree(fresh);
        return Wide(app.name) + L" está aberto: feche e tente de novo.";
    }
    if (!MoveFileExW(fresh.c_str(), folder.c_str(), 0)) {
        MoveFileExW(old.c_str(), folder.c_str(), 0);
        RemoveFolderTree(fresh);
        return L"Não deu para pôr " + Wide(app.name) + L" no lugar.";
    }
    RemoveFolderTree(old);
    return L"";
}

std::wstring RemoveStoreApp(const std::wstring& root, const std::string& id) {
    if (!SafeId(id)) return L"";
    const std::wstring folder = StoreAppFolder(root, id), old = folder + L".antiga";
    if (!IsDirectory(folder)) return L"";
    RemoveFolderTree(old);
    if (!MoveFileExW(folder.c_str(), old.c_str(), 0)) return L"Está aberto: feche e tente de novo.";
    RemoveFolderTree(old);
    return L"";
}

std::wstring SignStoreCatalog(const std::wstring& root, const std::wstring& path) {
    const std::string text = ReadBytes(path);
    if (text.rfind("ranga-apps 1", 0) != 0) return L"Não é um catálogo (a primeira linha tem que ser \"ranga-apps 1\").";
    // Every "app" must read back: a typo would make it vanish from every Ranga without a word.
    size_t declared = 0;
    for (size_t at = text.find("\napp "); at != std::string::npos; at = text.find("\napp ", at + 1)) ++declared;
    const size_t read = ReadCatalog(text).size();
    if (read != declared) return L"O catálogo tem " + std::to_wstring(declared) + L" apps, mas só " + std::to_wstring(read) + L" estão completos e seguros.";
    std::string signature;
    if (const std::wstring error = SignWithRangaKey(root, text, signature); !error.empty()) return error;
    const std::wstring out = path.substr(0, path.size() - (Lower(path).size() >= 4 && Lower(path).substr(path.size() - 4) == L".txt" ? 4 : 0)) + L".sig";
    FILE* file = _wfopen(out.c_str(), L"wb");
    if (file == nullptr) return L"Não deu para gravar " + out + L".";
    std::fwrite(signature.data(), 1, signature.size(), file);
    std::fclose(file);
    return L"";
}

}
