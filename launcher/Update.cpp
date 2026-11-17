#include "Update.hpp"
#include "Common.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace Ranga {

namespace {

// The public half of the key the hosting PC signs with (BCRYPT_ECCPUBLIC_BLOB, P-256), made on 17/11/2026; the
// private half is salas\chave-atualizacao.bin on that PC only.
constexpr const char* kPublicKey =
    "45435331200000006ef08f65b2c637d064250a6666e58a8434dc4370fc88748aa8fbdd0361a7ecf9a8bc7a04cf2e32f94214255c5dca20f2"
    "babfe9abaa7b8db9d7c4bc9d8552409e";
constexpr const wchar_t* kDefaultSource = L"";

std::wstring Ini(const std::wstring& root) { return root + L"\\engines.ini"; }
std::wstring Published(const std::wstring& root) { return root + L"\\salas\\atualizacao"; }
std::wstring PrivateKey(const std::wstring& root) { return root + L"\\salas\\chave-atualizacao.bin"; }
std::wstring Staging(const std::wstring& root) { return root + L"\\atualizacao-baixada"; }

std::string Hex(const unsigned char* data, size_t size) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < size; ++i) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 15];
    }
    return out;
}

std::string Unhex(const std::string& text) {
    std::string out;
    for (size_t i = 0; i + 1 < text.size(); i += 2) out += static_cast<char>(std::stoi(text.substr(i, 2), nullptr, 16));
    return out;
}

// SHA-256 of bytes in memory, or of a file read in pieces.
bool Sha256(const std::function<bool(BCRYPT_HASH_HANDLE)>& feed, unsigned char out[32]) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
              feed(hash) && BCryptFinishHash(hash, out, 32, 0) == 0;
    if (hash != nullptr) BCryptDestroyHash(hash);
    if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

std::string HashFile(const std::wstring& path) {
    unsigned char digest[32];
    const bool ok = Sha256([&](BCRYPT_HASH_HANDLE hash) {
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) return false;
        std::vector<unsigned char> block(1 << 20);
        bool good = true;
        for (size_t got; (got = std::fread(block.data(), 1, block.size(), file)) > 0;) good = good && BCryptHashData(hash, block.data(), static_cast<ULONG>(got), 0) == 0;
        std::fclose(file);
        return good;
    }, digest);
    return ok ? Hex(digest, 32) : "";
}

bool HashBytes(const std::string& bytes, unsigned char digest[32]) {
    return Sha256([&](BCRYPT_HASH_HANDLE hash) {
        return BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()), 0) == 0;
    }, digest);
}

BCRYPT_KEY_HANDLE ImportKey(BCRYPT_ALG_HANDLE algorithm, const std::string& blob, bool isPrivate) {
    BCRYPT_KEY_HANDLE key = nullptr;
    if (BCryptImportKeyPair(algorithm, nullptr, isPrivate ? BCRYPT_ECCPRIVATE_BLOB : BCRYPT_ECCPUBLIC_BLOB, &key,
                            reinterpret_cast<PUCHAR>(const_cast<char*>(blob.data())), static_cast<ULONG>(blob.size()), 0) != 0)
        return nullptr;
    return key;
}

bool Sign(const std::string& privateBlob, const std::string& message, std::string& signature) {
    unsigned char digest[32];
    if (!HashBytes(message, digest)) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0) return false;
    bool ok = false;
    if (BCRYPT_KEY_HANDLE key = ImportKey(algorithm, privateBlob, true)) {
        unsigned char out[64];
        ULONG size = 0;
        ok = BCryptSignHash(key, nullptr, digest, 32, out, sizeof(out), &size, 0) == 0;
        if (ok) signature.assign(reinterpret_cast<char*>(out), size);
        BCryptDestroyKey(key);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

bool Verify(const std::string& message, const std::string& signature) {
    unsigned char digest[32];
    if (signature.size() != 64 || !HashBytes(message, digest)) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0) return false;
    bool ok = false;
    if (BCRYPT_KEY_HANDLE key = ImportKey(algorithm, Unhex(kPublicKey), false)) {
        ok = BCryptVerifySignature(key, nullptr, digest, 32, reinterpret_cast<PUCHAR>(const_cast<char*>(signature.data())), 64, 0) == 0;
        BCryptDestroyKey(key);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

// What goes in an update: the programs and every core. Nothing from bios, saves, jogos or the rest.
std::vector<std::wstring> PublishedFiles(const std::wstring& root) {
    std::vector<std::wstring> files = {L"RangaEmu.exe", L"ranga_retro.exe"};
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((root + L"\\cores\\*_libretro.dll").c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) files.push_back(std::wstring(L"cores\\") + found.cFileName);
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    return files;
}

// A path from a list is used only when it is one of the kinds Ranga publishes: no "..", no drive, no other folder.
bool SafePath(const std::wstring& path) {
    if (path.empty() || path.find(L"..") != std::wstring::npos || path.find(L':') != std::wstring::npos || path[0] == L'\\' || path[0] == L'/') return false;
    if (path == L"RangaEmu.exe" || path == L"ranga_retro.exe") return true;
    const std::wstring lower = Lower(path), tail = L"_libretro.dll";
    return lower.rfind(L"cores\\", 0) == 0 && lower.find(L'\\', 6) == std::wstring::npos && lower.size() > 6 + tail.size() &&
           lower.compare(lower.size() - tail.size(), tail.size(), tail) == 0;
}

// A folder and everything in it (the update's own folders only).
void RemoveTree(const std::wstring& folder) {
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((folder + L"\\*").c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = found.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring full = folder + L"\\" + name;
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) RemoveTree(full);
            else DeleteFileW(full.c_str());
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    RemoveDirectoryW(folder.c_str());
}

void MakeParents(const std::wstring& path) {
    for (size_t at = path.find(L'\\', 3); at != std::wstring::npos; at = path.find(L'\\', at + 1)) CreateDirectoryW(path.substr(0, at).c_str(), nullptr);
}

// GET http://host:port<path>, the body to a file or to memory. Returns the HTTP status, 0 when unreachable.
int HttpGet(const std::wstring& source, const std::wstring& path, std::string* body, const std::wstring& toFile, const std::function<void(uint64_t)>& got) {
    std::wstring host = source;
    INTERNET_PORT port = 80;
    if (const size_t colon = source.rfind(L':'); colon != std::wstring::npos) {
        host = source.substr(0, colon);
        port = static_cast<INTERNET_PORT>(_wtoi(source.c_str() + colon + 1));
    }
    HINTERNET session = WinHttpOpen(L"RangaEmu", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) return 0;
    WinHttpSetTimeouts(session, 8000, 8000, 15000, 30000);
    int status = 0;
    if (HINTERNET connection = WinHttpConnect(session, host.c_str(), port, 0)) {
        if (HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0)) {
            if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)) {
                DWORD code = 0, size = sizeof(code);
                WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
                FILE* file = nullptr;
                if (code == 200 && !toFile.empty()) {
                    MakeParents(toFile);
                    _wfopen_s(&file, toFile.c_str(), L"wb");
                }
                bool complete = code != 200 || toFile.empty() || file != nullptr;
                uint64_t total = 0;
                std::vector<char> block(256 * 1024);
                for (;;) {
                    DWORD read = 0;
                    if (!WinHttpReadData(request, block.data(), static_cast<DWORD>(block.size()), &read)) { complete = false; break; }
                    if (read == 0) break;
                    if (file != nullptr) complete = complete && std::fwrite(block.data(), 1, read, file) == read;
                    else if (body != nullptr) body->append(block.data(), read);
                    total += read;
                    if (got) got(total);
                }
                if (file != nullptr) std::fclose(file);
                status = complete ? static_cast<int>(code) : 0;
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connection);
    }
    WinHttpCloseHandle(session);
    return status;
}

std::string UrlPath(const std::wstring& path) {
    std::string out = Utf8(path);
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

}

std::wstring UpdateSource(const std::wstring& root) { return ReadIni(Ini(root), L"Ranga", L"atualizacao", kDefaultSource); }

std::string InstalledUpdate(const std::wstring& root) { return Utf8(ReadIni(Ini(root), L"Ranga", L"versao_atualizacao", L"")); }

bool CanPublishUpdate(const std::wstring& root) { return Exists(PrivateKey(root)); }

std::wstring PublishUpdate(const std::wstring& root, const std::string& notes, std::string& version) {
    const std::string key = ReadBytes(PrivateKey(root));
    if (key.empty()) return L"Falta a chave de assinatura (salas\\chave-atualizacao.bin).";
    // The files are copied first, so a build made later does not change what friends are downloading.
    const std::wstring folder = Published(root), next = folder + L".nova";
    RemoveTree(next);
    std::string list;
    for (const std::wstring& path : PublishedFiles(root)) {
        const std::wstring to = next + L"\\files\\" + path;
        MakeParents(to);
        if (!CopyFileW((root + L"\\" + path).c_str(), to.c_str(), FALSE)) return L"Não deu para copiar " + path + L".";
        const std::string hash = HashFile(to);
        if (hash.empty()) return L"Não deu para ler " + path + L".";
        list += "arquivo " + hash + " " + std::to_string(FileSize(to)) + " " + UrlPath(path) + "\n";
    }
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y.%m.%d-%H%M", &local);
    version = stamp;
    std::string oneLine = notes;
    std::replace(oneLine.begin(), oneLine.end(), '\n', ' ');
    std::replace(oneLine.begin(), oneLine.end(), '\r', ' ');
    const std::string manifest = "ranga-atualizacao 1\nversao " + version + "\nnotas " + oneLine + "\n" + list;
    std::string signature;
    if (!Sign(key, manifest, signature)) return L"A assinatura falhou (a chave está estragada?).";
    for (const auto& [name, bytes] : {std::pair<const wchar_t*, const std::string*>{L"\\manifest.txt", &manifest}, {L"\\manifest.sig", &signature}}) {
        FILE* file = nullptr;
        if (_wfopen_s(&file, (next + name).c_str(), L"wb") != 0 || file == nullptr) return L"Não deu para gravar a lista.";
        std::fwrite(bytes->data(), 1, bytes->size(), file);
        std::fclose(file);
    }
    // The new set takes the old one's place in two renames; the server reads the folder per request.
    const std::wstring old = folder + L".antiga";
    RemoveTree(old);
    MoveFileExW(folder.c_str(), old.c_str(), 0);
    if (!MoveFileExW(next.c_str(), folder.c_str(), 0)) return L"Não deu para pôr a versão no lugar.";
    RemoveTree(old);
    EnsureUnicodeIni(Ini(root));
    WritePrivateProfileStringW(L"Ranga", L"versao_atualizacao", Wide(version).c_str(), Ini(root).c_str());
    return L"";
}

UpdateInfo CheckUpdate(const std::wstring& root) {
    UpdateInfo info;
    const std::wstring source = UpdateSource(root);
    std::string manifest, signature;
    if (HttpGet(source, L"/ranga/manifest", &manifest, L"", nullptr) != 200 || HttpGet(source, L"/ranga/manifest.sig", &signature, L"", nullptr) != 200) {
        info.error = L"O PC que hospeda não respondeu (desligado?).";
        return info;
    }
    if (!Verify(manifest, signature)) {
        info.error = L"A lista de atualização não tem a assinatura certa: ignorada.";
        return info;
    }
    size_t start = 0;
    while (start < manifest.size()) {
        size_t end = manifest.find('\n', start);
        if (end == std::string::npos) end = manifest.size();
        const std::string line = manifest.substr(start, end - start);
        start = end + 1;
        if (line.rfind("versao ", 0) == 0) info.version = line.substr(7);
        else if (line.rfind("notas ", 0) == 0) info.notes = line.substr(6);
        else if (line.rfind("arquivo ", 0) == 0) {
            const size_t a = 8, b = line.find(' ', a), c = b == std::string::npos ? b : line.find(' ', b + 1);
            if (c == std::string::npos) continue;
            UpdateFile file;
            file.sha256 = line.substr(a, b - a);
            file.size = std::strtoull(line.c_str() + b + 1, nullptr, 10);
            file.path = Wide(line.substr(c + 1));
            std::replace(file.path.begin(), file.path.end(), L'/', L'\\');
            if (!SafePath(file.path) || file.sha256.size() != 64) continue;
            const std::wstring local = root + L"\\" + file.path;
            if (Exists(local) && static_cast<uint64_t>(FileSize(local)) == file.size && HashFile(local) == file.sha256) continue;
            info.download += file.size;
            info.changed.push_back(file);
        }
    }
    if (info.version.empty()) info.error = L"A lista de atualização veio vazia.";
    return info;
}

std::wstring DownloadUpdate(const std::wstring& root, const UpdateInfo& info, const std::function<void(uint64_t, uint64_t)>& progress) {
    const std::wstring source = UpdateSource(root), staging = Staging(root);
    uint64_t done = 0;
    for (const UpdateFile& file : info.changed) {
        const std::wstring to = staging + L"\\" + file.path;
        const int status = HttpGet(source, L"/ranga/f/" + Wide(UrlPath(file.path)), nullptr, to, [&](uint64_t got) {
            if (progress) progress(done + got, info.download);
        });
        if (status != 200) return L"O download de " + file.path + L" parou (o PC que hospeda caiu?).";
        if (HashFile(to) != file.sha256) return L"O arquivo " + file.path + L" chegou diferente do que foi publicado: nada foi trocado.";
        done += file.size;
    }
    return L"";
}

std::wstring ApplyUpdate(const std::wstring& root, const UpdateInfo& info) {
    const std::wstring staging = Staging(root);
    for (const UpdateFile& file : info.changed) {
        const std::wstring target = root + L"\\" + file.path, ready = staging + L"\\" + file.path;
        if (!Exists(ready)) return L"Falta o arquivo baixado " + file.path + L".";
        if (Exists(target) && !MoveFileExW(target.c_str(), (target + L".old").c_str(), MOVEFILE_REPLACE_EXISTING))
            return L"O arquivo " + file.path + L" está em uso: feche o jogo e tente de novo.";
        if (!MoveFileExW(ready.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            MoveFileExW((target + L".old").c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
            return L"Não deu para trocar " + file.path + L".";
        }
    }
    EnsureUnicodeIni(Ini(root));
    WritePrivateProfileStringW(L"Ranga", L"versao_atualizacao", Wide(info.version).c_str(), Ini(root).c_str());
    return L"";
}

void CleanUpdateLeftovers(const std::wstring& root) {
    for (const std::wstring& folder : {root, root + L"\\cores"}) {
        WIN32_FIND_DATAW found;
        HANDLE search = FindFirstFileW((folder + L"\\*.old").c_str(), &found);
        if (search == INVALID_HANDLE_VALUE) continue;
        do {
            // The old RangaEmu.exe is still held by Windows for a moment after it closes: a few more tries.
            const std::wstring old = folder + L"\\" + found.cFileName;
            for (int attempt = 0; attempt < 10 && !DeleteFileW(old.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND; ++attempt) Sleep(300);
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    const std::wstring staging = Staging(root);
    if (IsDirectory(staging)) RemoveTree(staging);
}

}
