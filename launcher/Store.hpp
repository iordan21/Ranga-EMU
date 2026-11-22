#ifndef LAUNCHER_STORE_HPP
#define LAUNCHER_STORE_HPP

// Apps: a small store of the owner's own Windows programs (the first is Ranga TV, the IPTV player in
// C:\projetos\Ranga "app pc"), each turned on from Ranga and opened from it, with the gamepad passed to it.
//
// The list is a catalog the owner signs with the same key as Ranga's updates (salas\chave-atualizacao.bin, only on
// that PC) and publishes as the latest release of github.com/iordan21/Ranga-Apps (catalogo.txt and catalogo.sig).
// Every app lives in a repository of its own and its zip is a release asset there; the catalog names the zip's
// address, its SHA-256 and size, and the program inside. Ranga shows only a catalog signed with that key, downloads
// only from GitHub (iordan21) and installs only a zip whose hash is the catalog's. An installed app sits in
// apps\<id>\, with its catalog entry beside it (ranga-app.txt), so it opens even with no internet.
//
// The catalog (UTF-8, one "key value" per line; every app starts at "app"):
//   ranga-apps 1
//   app ranga-tv
//   nome Ranga TV
//   versao 1.2.0
//   descricao O player de IPTV do Ranga...
//   zip https://github.com/iordan21/Ranga-TV/releases/download/v1.2.0/ranga-tv-1.2.0.zip
//   sha256 <64 hex>
//   tamanho <bytes>
//   exe Ranga.exe
//   args --ranga-emu
//   args_tv --tela-cheia --escala=1.35

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Ranga {

struct StoreApp {
    std::string id, name, version, description;
    std::wstring zip, exe, args, tvArgs;
    std::string sha256;
    uint64_t size = 0;
    // Here: whether it is installed and which version; newer when the catalog has another one.
    bool installed = false, newer = false;
    std::string installedVersion;
};

struct StoreCatalog {
    std::vector<StoreApp> apps; // the catalog's, with what is installed here; else only the installed ones
    std::wstring error;         // why the catalog did not come (the installed apps still show)
};

// The catalog's address: engines.ini [Ranga] loja, else Ranga-Apps' latest release.
std::wstring StoreAddress(const std::wstring& root);
// The signed catalog and what is installed (network: off the window's thread).
StoreCatalog FetchStore(const std::wstring& root);
// Only the installed apps, from their ranga-app.txt (no network).
std::vector<StoreApp> InstalledApps(const std::wstring& root);
// A catalog's apps (no signature check; FetchStore checks it before). Unsafe entries are left out.
std::vector<StoreApp> ReadCatalog(const std::string& text);

std::wstring StoreAppFolder(const std::wstring& root, const std::string& id);
std::wstring StoreAppExe(const std::wstring& root, const StoreApp& app);
// Downloads, checks and unzips it (or the new version over the old). The app must be closed.
std::wstring InstallStoreApp(const std::wstring& root, const StoreApp& app, const std::function<void(uint64_t, uint64_t)>& progress);
// Removes it (what it keeps elsewhere, such as Ranga TV's lists in %APPDATA%, stays).
std::wstring RemoveStoreApp(const std::wstring& root, const std::string& id);

// The hosting PC (RangaEmu.exe --assinar-catalogo <arquivo>): writes <arquivo>.sig beside it, after checking the
// catalog reads back whole.
std::wstring SignStoreCatalog(const std::wstring& root, const std::wstring& path);

}

#endif
