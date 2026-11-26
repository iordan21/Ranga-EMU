#include "Update.hpp"
#include "Common.hpp"
#include "Enderecos.hpp"

#include <windows.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <wincred.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace Ranga {

namespace {

// The public half of the key the hosting PC signs with (BCRYPT_ECCPUBLIC_BLOB, P-256), made on 17/11/2026; the
// private half is salas\chave-atualizacao.bin on that PC only.
constexpr const char* kPublicKey =
    "45435331200000006ef08f65b2c637d064250a6666e58a8434dc4370fc88748aa8fbdd0361a7ecf9a8bc7a04cf2e32f94214255c5dca20f2"
    "babfe9abaa7b8db9d7c4bc9d8552409e";
constexpr const wchar_t* kDefaultSource = RANGA_FONTE_ATUALIZACAO;
constexpr const wchar_t* kGithubRepo = L"iordan21/Ranga-EMU";
constexpr const wchar_t* kGithubBeta = L"https://github.com/iordan21/Ranga-EMU/releases/download/beta";
constexpr const wchar_t* kGithubSource = L"https://github.com/iordan21/Ranga-EMU/releases/latest/download";

std::wstring Ini(const std::wstring& root) { return root + L"\\engines.ini"; }
std::wstring Published(const std::wstring& root, bool beta = false) { return root + (beta ? L"\\salas\\atualizacao-beta" : L"\\salas\\atualizacao"); }
std::wstring PrivateKey(const std::wstring& root) { return root + L"\\salas\\chave-atualizacao.bin"; }
// The same key kept encrypted with a password only the owner knows (SealKey): a program that copies the file gets
// nothing it can sign with.
std::wstring SealedKey(const std::wstring& root) { return root + L"\\salas\\chave-atualizacao.cifrada"; }
std::wstring Staging(const std::wstring& root) { return root + L"\\atualizacao-baixada"; }
std::wstring PreviousFolder(const std::wstring& root) { return root + L"\\versao-anterior"; }

bool WriteBytes(const std::wstring& path, const std::string& bytes) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    return std::fclose(file) == 0 && ok;
}

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

// ---- The key at rest: "RKEY1", salt (16), nonce (12), tag (16), then the key blob encrypted with AES-256-GCM under
// PBKDF2-HMAC-SHA256(password, salt, kSealRounds). The rounds make each guess of the password cost a few tenths of
// a second.
constexpr ULONGLONG kSealRounds = 600000;
constexpr size_t kSaltBytes = 16, kNonceBytes = 12, kTagBytes = 16;

bool Stretch(const std::wstring& password, const unsigned char* salt, unsigned char out[32]) {
    const std::string bytes = Utf8(password);
    BCRYPT_ALG_HANDLE hmac = nullptr;
    if (BCryptOpenAlgorithmProvider(&hmac, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) return false;
    const bool ok = BCryptDeriveKeyPBKDF2(hmac, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()),
                                          const_cast<PUCHAR>(salt), kSaltBytes, kSealRounds, out, 32, 0) == 0;
    BCryptCloseAlgorithmProvider(hmac, 0);
    return ok;
}

// AES-256-GCM one way or the other; false when the tag does not match (wrong password, or the file was changed).
bool Gcm(bool encrypt, const unsigned char key[32], const unsigned char* nonce, unsigned char* tag, const std::string& in, std::string& out) {
    BCRYPT_ALG_HANDLE aes = nullptr;
    BCRYPT_KEY_HANDLE handle = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&aes, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0 &&
              BCryptSetProperty(aes, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                                sizeof(BCRYPT_CHAIN_MODE_GCM), 0) == 0 &&
              BCryptGenerateSymmetricKey(aes, &handle, nullptr, 0, const_cast<PUCHAR>(key), 32, 0) == 0;
    if (ok) {
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = const_cast<PUCHAR>(nonce);
        info.cbNonce = kNonceBytes;
        info.pbTag = tag;
        info.cbTag = kTagBytes;
        out.assign(in.size(), '\0');
        ULONG done = 0;
        const auto data = reinterpret_cast<PUCHAR>(const_cast<char*>(in.data()));
        const auto into = reinterpret_cast<PUCHAR>(out.data());
        ok = (encrypt ? BCryptEncrypt(handle, data, static_cast<ULONG>(in.size()), &info, nullptr, 0, into, static_cast<ULONG>(out.size()), &done, 0)
                      : BCryptDecrypt(handle, data, static_cast<ULONG>(in.size()), &info, nullptr, 0, into, static_cast<ULONG>(out.size()), &done, 0)) == 0;
    }
    if (handle != nullptr) BCryptDestroyKey(handle);
    if (aes != nullptr) BCryptCloseAlgorithmProvider(aes, 0);
    if (!ok) out.clear();
    return ok;
}

std::string Seal(const std::string& keyBlob, const std::wstring& password) {
    std::string out = "RKEY1";
    unsigned char salt[kSaltBytes], nonce[kNonceBytes], tag[kTagBytes] = {}, key[32];
    if (BCryptGenRandom(nullptr, salt, sizeof(salt), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0 ||
        BCryptGenRandom(nullptr, nonce, sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0 || !Stretch(password, salt, key))
        return "";
    std::string sealed;
    const bool ok = Gcm(true, key, nonce, tag, keyBlob, sealed);
    SecureZeroMemory(key, sizeof(key));
    if (!ok) return "";
    out.append(reinterpret_cast<char*>(salt), kSaltBytes).append(reinterpret_cast<char*>(nonce), kNonceBytes).append(reinterpret_cast<char*>(tag), kTagBytes);
    return out + sealed;
}

std::string Unseal(const std::string& file, const std::wstring& password) {
    const size_t head = 5 + kSaltBytes + kNonceBytes + kTagBytes;
    if (file.size() <= head || file.compare(0, 5, "RKEY1") != 0) return "";
    const auto bytes = reinterpret_cast<const unsigned char*>(file.data());
    unsigned char key[32], tag[kTagBytes];
    std::memcpy(tag, bytes + 5 + kSaltBytes + kNonceBytes, kTagBytes);
    if (!Stretch(password, bytes + 5, key)) return "";
    std::string blob;
    Gcm(false, key, bytes + 5 + kSaltBytes, tag, file.substr(head), blob);
    SecureZeroMemory(key, sizeof(key));
    return blob;
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

// The owner's password, asked in Windows' own box (nothing is kept). Empty: cancelled.
std::wstring AskPassword(const std::wstring& message) {
    CREDUI_INFOW info{sizeof(info), nullptr, message.c_str(), L"Ranga Emu: chave de atualização", nullptr};
    wchar_t user[CREDUI_MAX_USERNAME_LENGTH + 1] = L"Ranga";
    wchar_t password[CREDUI_MAX_PASSWORD_LENGTH + 1] = {};
    BOOL save = FALSE;
    const DWORD answer = CredUIPromptForCredentialsW(&info, L"RangaEmu-chave", nullptr, 0, user, CREDUI_MAX_USERNAME_LENGTH + 1, password,
                                                     CREDUI_MAX_PASSWORD_LENGTH + 1, &save,
                                                     CREDUI_FLAGS_GENERIC_CREDENTIALS | CREDUI_FLAGS_DO_NOT_PERSIST | CREDUI_FLAGS_ALWAYS_SHOW_UI |
                                                         CREDUI_FLAGS_KEEP_USERNAME | CREDUI_FLAGS_PASSWORD_ONLY_OK);
    std::wstring out = answer == NO_ERROR ? password : L"";
    SecureZeroMemory(password, sizeof(password));
    return out;
}

// The private key to sign with: the plain file while it is there, else the sealed one with the owner's password
// (three tries). Empty: why not, in error.
std::string LoadSigningKey(const std::wstring& root, std::wstring& error) {
    if (std::string plain = ReadBytes(PrivateKey(root)); !plain.empty()) return plain;
    const std::string sealed = ReadBytes(SealedKey(root));
    if (sealed.empty()) {
        error = L"Falta a chave de assinatura (salas\\chave-atualizacao.cifrada): só o PC que publica assina.";
        return "";
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        const std::wstring password = AskPassword(attempt == 0 ? L"A senha da chave que assina as atualizações do Ranga." : L"Senha errada. Tente de novo.");
        if (password.empty()) {
            error = L"Publicação cancelada: a senha da chave não foi digitada.";
            return "";
        }
        if (std::string blob = Unseal(sealed, password); !blob.empty()) return blob;
    }
    error = L"Senha da chave errada três vezes: nada foi publicado.";
    return "";
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
// A file of an update as a release asset: its path with "--" for each folder ("cores--fbneo_libretro.dll").
std::wstring AssetName(const std::wstring& path) {
    std::wstring out;
    for (const wchar_t c : path) {
        if (c == L'\\' || c == L'/') out += L"--";
        else out += c;
    }
    return out;
}

// GET from a source: a table server ("host:port", the paths as they are) or a release folder ("https://...", where
// /ranga/manifest is manifest.txt and /ranga/f/<path> is the asset AssetName(path); redirects are followed).
int HttpGet(const std::wstring& source, const std::wstring& request, std::string* body, const std::wstring& toFile, const std::function<void(uint64_t)>& got) {
#ifdef RANGA_ALPHA
    return 0;
#endif
    std::wstring host = source, path = request;
    INTERNET_PORT port = 80;
    DWORD flags = 0;
    const bool secure = source.rfind(L"https://", 0) == 0;
    if (secure || source.rfind(L"http://", 0) == 0) {
        const std::wstring rest = source.substr(secure ? 8 : 7);
        const size_t slash = rest.find(L'/');
        host = rest.substr(0, slash);
        const std::wstring base = slash == std::wstring::npos ? L"" : rest.substr(slash);
        port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
        // An address with its port ("http://127.0.0.1:8799/x"): the port apart from the host.
        if (const size_t colon = host.rfind(L':'); colon != std::wstring::npos) {
            port = static_cast<INTERNET_PORT>(_wtoi(host.c_str() + colon + 1));
            host.resize(colon);
        }
        if (secure) flags = WINHTTP_FLAG_SECURE;
        if (request == L"/ranga/manifest") path = L"/manifest.txt";
        else if (request == L"/ranga/manifest.sig") path = L"/manifest.sig";
        else if (request.rfind(L"/ranga/f/", 0) == 0) path = L"/" + AssetName(request.substr(9));
        path = base + path;
    } else if (const size_t colon = source.rfind(L':'); colon != std::wstring::npos) {
        host = source.substr(0, colon);
        port = static_cast<INTERNET_PORT>(_wtoi(source.c_str() + colon + 1));
    }
    HINTERNET session = WinHttpOpen(L"RangaEmu", secure ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr && secure) session = WinHttpOpen(L"RangaEmu", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) return 0;
    WinHttpSetTimeouts(session, 8000, 8000, 15000, 30000);
    int status = 0;
    if (HINTERNET connection = WinHttpConnect(session, host.c_str(), port, 0)) {
        if (HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags)) {
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

// The lines of a signed list: its version, notes and files (only the safe paths).
void ReadManifest(const std::string& manifest, std::string& version, std::string& notes, std::vector<UpdateFile>& files, std::string* name = nullptr, bool* beta = nullptr) {
    size_t start = 0;
    while (start < manifest.size()) {
        size_t end = manifest.find('\n', start);
        if (end == std::string::npos) end = manifest.size();
        const std::string line = manifest.substr(start, end - start);
        start = end + 1;
        if (line.rfind("versao ", 0) == 0) version = line.substr(7);
        else if (line.rfind("nome ", 0) == 0 && name != nullptr) *name = line.substr(5);
        else if (line == "canal beta" && beta != nullptr) *beta = true;
        else if (line.rfind("notas ", 0) == 0) notes = line.substr(6);
        else if (line.rfind("arquivo ", 0) == 0) {
            const size_t a = 8, b = line.find(' ', a), c = b == std::string::npos ? b : line.find(' ', b + 1);
            if (c == std::string::npos) continue;
            UpdateFile file;
            file.sha256 = line.substr(a, b - a);
            file.size = std::strtoull(line.c_str() + b + 1, nullptr, 10);
            file.path = Wide(line.substr(c + 1));
            std::replace(file.path.begin(), file.path.end(), L'/', L'\\');
            if (SafePath(file.path) && file.sha256.size() == 64) files.push_back(file);
        }
    }
}

std::string UrlPath(const std::wstring& path) {
    std::string out = Utf8(path);
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

// The newest signed list among the sources (each fetched and checked); source says which one. Empty manifest: none.
std::wstring FetchManifest(const std::wstring& root, std::string& manifest, std::wstring& source) {
    manifest.clear();
    source.clear();
    std::string best;
    bool answered = false, badSignature = false;
    for (const std::wstring& from : UpdateSources(root)) {
        std::string text, signature;
        if (HttpGet(from, L"/ranga/manifest", &text, L"", nullptr) != 200 || HttpGet(from, L"/ranga/manifest.sig", &signature, L"", nullptr) != 200) continue;
        answered = true;
        if (!Verify(text, signature)) {
            badSignature = true;
            continue;
        }
        std::string version, notes;
        std::vector<UpdateFile> files;
        bool beta = false;
        ReadManifest(text, version, notes, files, nullptr, &beta);
        if (beta && !BetaChannel(root)) continue;
        // "AAAA.MM.DD-HHMM" sorts as text.
        if (!version.empty() && version > best) {
            best = version;
            manifest = text;
            source = from;
        }
    }
    if (!manifest.empty()) return L"";
    if (badSignature) return L"A lista de atualização não tem a assinatura certa: ignorada.";
    if (!answered) return L"Nem o GitHub nem o PC que hospeda responderam (sem internet?).";
    return L"A lista de atualização veio vazia.";
}

// Runs a command hidden and waits; out gets what it printed. The exit code, or -1 when it did not start.
int RunCaptured(std::wstring command, const std::wstring& folder, std::string& out) {
    out.clear();
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring logPath = std::wstring(temp) + L"ranga-comando-" + std::to_wstring(GetCurrentProcessId()) + L".log";
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = log;
    startup.hStdError = log;
    PROCESS_INFORMATION process{};
    const bool started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, folder.empty() ? nullptr : folder.c_str(),
                                        &startup, &process);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!started) return -1;
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    out = ReadBytes(logPath);
    DeleteFileW(logPath.c_str());
    return static_cast<int>(code);
}

}

std::vector<std::wstring> UpdateSources(const std::wstring& root) {
    std::vector<std::wstring> sources;
    const auto add = [&](std::wstring source) {
        while (!source.empty() && (source.back() == L' ' || source.back() == L'/')) source.pop_back();
        while (!source.empty() && source.front() == L' ') source.erase(0, 1);
        if (!source.empty() && std::find(sources.begin(), sources.end(), source) == sources.end()) sources.push_back(source);
    };
    const std::wstring chosen = ReadIni(Ini(root), L"Ranga", L"atualizacao", L"");
    for (size_t start = 0; start <= chosen.size();) {
        size_t end = chosen.find(L';', start);
        if (end == std::wstring::npos) end = chosen.size();
        add(chosen.substr(start, end - start));
        start = end + 1;
    }
    if (BetaChannel(root)) add(kGithubBeta);
    add(kGithubSource);
    add(kDefaultSource);
    return sources;
}

std::wstring UpdateSource(const std::wstring& root) { return UpdateSources(root).front(); }

std::string InstalledUpdate(const std::wstring& root) { return Utf8(ReadIni(Ini(root), L"Ranga", L"versao_atualizacao", L"")); }

bool CanPublishUpdate(const std::wstring& root) { return Exists(PrivateKey(root)); }

// "1.0.0": three numbers with dots, nothing else.
bool VersionName(const std::string& text) {
    int parts = 1;
    if (text.empty() || text.front() == '.' || text.back() == '.') return false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '.') {
            if (text[i - 1] == '.') return false;
            ++parts;
        } else if (text[i] < '0' || text[i] > '9') {
            return false;
        }
    }
    return parts == 3;
}

std::string InstalledVersionName(const std::wstring& root) { return Utf8(ReadIni(Ini(root), L"Ranga", L"versao_nome", L"")); }

std::wstring PublishUpdate(const std::wstring& root, const std::string& notes, std::string& version, const std::string& wanted, bool beta) {
#ifdef RANGA_ALPHA
    return L"O Ranga Alpha não publica.";
#endif
    std::wstring keyError;
    const std::string key = LoadSigningKey(root, keyError);
    if (key.empty()) return keyError;
    // The files are copied first, so a build made later does not change what friends are downloading.
    const std::wstring folder = Published(root, beta), next = folder + L".nova";
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
    // The number people see (1.0.0): the one asked for, else the last one with its last part one up; the first is
    // 1.0.0. The date above stays the version Ranga compares, so a Ranga that knows no numbers still updates.
    const std::wstring numberFile = root + L"\\salas\\versao.txt";
    std::string number = VersionName(wanted) ? wanted : "";
    if (number.empty()) {
        std::string last = ReadBytes(numberFile);
        while (!last.empty() && (last.back() == '\n' || last.back() == '\r' || last.back() == ' ')) last.pop_back();
        if (VersionName(last)) {
            const size_t dot = last.rfind('.');
            number = last.substr(0, dot + 1) + std::to_string(std::atoi(last.c_str() + dot + 1) + 1);
        } else {
            number = "1.0.0";
        }
    }
    std::string oneLine = notes;
    std::replace(oneLine.begin(), oneLine.end(), '\n', ' ');
    std::replace(oneLine.begin(), oneLine.end(), '\r', ' ');
    oneLine.erase(std::remove(oneLine.begin(), oneLine.end(), '\t'), oneLine.end());
    const std::string manifest = "ranga-atualizacao 1\nversao " + version + "\nnome " + number + (beta ? " beta\ncanal beta" : "") + "\nnotas " + oneLine + "\n" + list;
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
    // A beta spends no number and makes no news; this PC only shows what it runs.
    if (beta) {
        WritePrivateProfileStringW(L"Ranga", L"versao_nome", Wide(number + " beta").c_str(), Ini(root).c_str());
        return L"";
    }
    WritePrivateProfileStringW(L"Ranga", L"versao_atualizacao", Wide(version).c_str(), Ini(root).c_str());
    WritePrivateProfileStringW(L"Ranga", L"versao_nome", Wide(number).c_str(), Ini(root).c_str());
    if (FILE* file = _wfopen(numberFile.c_str(), L"wb")) {
        std::fwrite(number.data(), 1, number.size(), file);
        std::fclose(file);
    }
    // The news board (Novidades in every Ranga): one line per version, "version<TAB>notes<TAB>number", oldest first,
    // kept on the hosting PC and sent to GitHub with each release (GitHub keeps only the newest three releases).
    if (FILE* history = _wfopen((root + L"\\salas\\novidades.txt").c_str(), L"ab")) {
        const std::string line = version + "\t" + oneLine + "\t" + number + "\n";
        std::fwrite(line.data(), 1, line.size(), history);
        std::fclose(history);
    }
    return L"";
}

UpdateInfo CheckUpdate(const std::wstring& root) {
    UpdateInfo info;
    std::string manifest;
    info.error = FetchManifest(root, manifest, info.source);
    if (!info.error.empty()) return info;
    std::vector<UpdateFile> files;
    ReadManifest(manifest, info.version, info.notes, files, &info.name);
    info.older = !info.version.empty() && info.version < InstalledUpdate(root);
    info.declined = !info.version.empty() && info.version <= DeclinedUpdate(root);
    if (info.older || info.declined) return info;
    for (const UpdateFile& file : files) {
        const std::wstring local = root + L"\\" + file.path;
        if (Exists(local) && static_cast<uint64_t>(FileSize(local)) == file.size && HashFile(local) == file.sha256) continue;
        info.download += file.size;
        info.changed.push_back(file);
    }
    if (info.version.empty()) info.error = L"A lista de atualização veio vazia.";
    return info;
}

namespace {

// What a friend's package leaves out, relative to Ranga's folder in lower case: whole folders, and single files.
const std::vector<std::wstring> kPackageSkipFolders = {
    L"pacote-amigo-temp", L"jogos", L"bios", L"backups", L"saves", L"perfil", L"covers", L"salas", L"tools", L"addons", L"reshade-shaders", L"logs",
    L"atualizacao-baixada", L"versao-anterior", L"versao-anterior.antiga", L"emuladores\\_downloads", L"emuladores\\eden\\user\\keys", L"emuladores\\eden\\user\\nand",
    L"emuladores\\eden\\user\\shader", L"emuladores\\eden\\user\\sdmc", L"emuladores\\rpcs3\\dev_flash", L"emuladores\\rpcs3\\dev_hdd0",
    L"emuladores\\cemu\\cemu_2.6\\portable\\mlc01", L"emuladores\\xenia\\content", L"emuladores\\retroarch\\retroarch-win64\\saves",
    L"emuladores\\retroarch\\retroarch-win64\\states", L"emuladores\\retroarch\\retroarch-win64\\system",
    L"emuladores\\pcsx2\\memcards", L"emuladores\\pcsx2\\sstates", L"emuladores\\pcsx2\\bios", L"emuladores\\duckstation\\memcards",
    L"emuladores\\duckstation\\bios", L"emuladores\\dolphin\\dolphin-x64\\user"};
const std::vector<std::wstring> kPackageSkipFiles = {L"engines.ini", L"library.ini", L"salas.ini", L"emuladores\\xemu\\eeprom.bin"};

bool PackageSkips(const std::wstring& rel, bool folder) {
    const std::wstring low = Lower(rel);
    if (folder) {
        // Azahar's own user folder (this PC's 3DS system settings, logs) sits under a folder named by its version.
        const std::wstring user = L"\\user";
        if (low.rfind(L"emuladores\\azahar\\", 0) == 0 && low.size() > user.size() && low.compare(low.size() - user.size(), user.size(), user) == 0) return true;
        return std::find(kPackageSkipFolders.begin(), kPackageSkipFolders.end(), low) != kPackageSkipFolders.end();
    }
    if (std::find(kPackageSkipFiles.begin(), kPackageSkipFiles.end(), low) != kPackageSkipFiles.end()) return true;
    // Games, disc images, keys and logs, wherever they are; and the DLSS kit of this PC (NVIDIA's, not to hand out).
    for (const wchar_t* end : {L".iso", L".chd", L".cue", L".nsp", L".xci", L".nsz", L".3ds", L".cia", L".wbfs", L".rvz", L".pkg", L".keys",
                               L".log", L".old", L".bak", L".addon64", L".rani"}) {
        const size_t n = wcslen(end);
        if (low.size() > n && low.compare(low.size() - n, n, end) == 0) return true;
    }
    return low.rfind(L"nvngx", 0) == 0 || low.rfind(L"reshade", 0) == 0 || low.rfind(L"dlss5", 0) == 0 || low.rfind(L"renodx", 0) == 0;
}

// Puts every file that goes into the package under to, as a hard link (instant, no space) or a copy on another disk.
bool PackageLink(const std::wstring& root, const std::wstring& rel, const std::wstring& to, int& count) {
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((root + (rel.empty() ? L"" : L"\\" + rel) + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return true;
    do {
        const std::wstring name = found.cFileName;
        if (name == L"." || name == L".." || (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;
        const std::wstring path = rel.empty() ? name : rel + L"\\" + name;
        const bool folder = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (PackageSkips(path, folder)) continue;
        const std::wstring from = root + L"\\" + path, target = to + L"\\" + path;
        if (folder) {
            CreateDirectoryW(target.c_str(), nullptr);
            if (!PackageLink(root, path, to, count)) {
                FindClose(search);
                return false;
            }
        } else if (CreateHardLinkW(target.c_str(), from.c_str(), nullptr) || CopyFileW(from.c_str(), target.c_str(), FALSE)) {
            ++count;
        } else {
            FindClose(search);
            return false;
        }
    } while (FindNextFileW(search, &found));
    FindClose(search);
    return true;
}

}

std::wstring MakeFriendPackage(const std::wstring& root, const std::wstring& folder, std::wstring& made) {
    made.clear();
    if (!IsDirectory(folder)) return L"A pasta escolhida não existe.";
    if (Lower(folder + L"\\").rfind(Lower(root + L"\\"), 0) == 0) return L"Escolha uma pasta fora da pasta do Ranga.";
    // The package is put together as staging\\Ranga (links to the real files), then zipped whole.
    const std::wstring staging = root + L"\\pacote-amigo-temp", tree = staging + L"\\Ranga";
    RemoveTree(staging);
    CreateDirectoryW(staging.c_str(), nullptr);
    CreateDirectoryW(tree.c_str(), nullptr);

    // This PC's engines.ini without what only this PC has.
    const std::wstring ini = tree + L"\\engines.ini";
    if (!CopyFileW((root + L"\\engines.ini").c_str(), ini.c_str(), FALSE)) {
        RemoveTree(staging);
        return L"Não deu para preparar o engines.ini.";
    }
    for (const wchar_t* key : {L"servidor", L"grupo", L"backup", L"backup_quando", L"qualidade", L"raiz", L"perfis", L"central", L"beta", L"versao_pulada"})
        WritePrivateProfileStringW(L"Ranga", key, nullptr, ini.c_str());
    const std::string readme =
        "RANGA EMU - instalação completa\r\n"
        "===============================\r\n\r\n"
        "1. Extraia este zip onde quiser (por exemplo em C:\\): ele cria a pasta Ranga.\r\n"
        "2. Abra o RangaEmu.exe. Se o Windows avisar: \"Mais informações\" -> \"Executar assim mesmo\".\r\n"
        "3. Adicione os seus jogos em \"+ Adicionar jogo\".\r\n\r\n"
        "O que NÃO vem (é de cada um): jogos, BIOS, firmware e chaves dos consoles. Os consoles que pedem BIOS mostram\r\n"
        "o que falta e onde colocar na página do console.\r\n\r\n"
        "Depois disso o Ranga se atualiza sozinho com o PC de quem hospeda ligado, e Emuladores -> Arquivos do Ranga\r\n"
        "confere e conserta os arquivos dele.\r\n";
    FILE* note = nullptr;
    if (_wfopen_s(&note, (tree + L"\\LEIA-ME.txt").c_str(), L"wb") == 0 && note != nullptr) {
        std::fwrite(readme.data(), 1, readme.size(), note);
        std::fclose(note);
    }
    int count = 0;
    if (!PackageLink(root, L"", tree, count)) {
        RemoveTree(staging);
        return L"Não deu para juntar os arquivos do pacote (disco cheio?).";
    }

    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    wchar_t stamp[32];
    wcsftime(stamp, 32, L"%Y-%m-%d", &local);
    const std::wstring zip = folder + L"\\Ranga-completo-" + stamp + L".zip";
    DeleteFileW(zip.c_str());
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(system) + L"\\tar.exe\" -a -c -f \"" + zip + L"\" --options zip:hdrcharset=UTF-8 -C \"" + staging + L"\" Ranga";
    // What tar says goes to a file, so a failure can tell why.
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    const std::wstring logPath = staging + L"\\tar.log";
    HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = log;
    startup.hStdError = log;
    PROCESS_INFORMATION process{};
    const bool started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &process);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!started) {
        RemoveTree(staging);
        return L"Não deu para abrir o tar.exe do Windows.";
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    std::string said = ReadBytes(logPath);
    RemoveTree(staging);
    if (code != 0 || !Exists(zip)) {
        DeleteFileW(zip.c_str());
        if (said.size() > 400) said = said.substr(said.size() - 400);
        return L"O tar.exe não terminou o zip (código " + std::to_wstring(code) + L"): " + Wide(said);
    }
    made = zip;
    return L"";
}

std::wstring PublishToGitHub(const std::wstring& root, std::string& version, bool beta) {
#ifdef RANGA_ALPHA
    return L"O Ranga Alpha não publica.";
#endif
    const std::wstring published = Published(root, beta);
    const std::string manifest = ReadBytes(published + L"\\manifest.txt");
    std::string notes, number;
    std::vector<UpdateFile> files;
    ReadManifest(manifest, version, notes, files, &number);
    if (version.empty() || files.empty()) return L"Nenhuma versão publicada para mandar.";
    wchar_t found[MAX_PATH] = {};
    std::wstring gh = SearchPathW(nullptr, L"gh.exe", nullptr, MAX_PATH, found, nullptr) != 0 ? found : L"C:\\Program Files\\GitHub CLI\\gh.exe";
    if (!Exists(gh)) return L"Falta o GitHub CLI (gh) neste PC: a versão ficou só nesta sala.";
    SetEnvironmentVariableW(L"GH_PROMPT_DISABLED", L"1");

    // The assets: each file under its AssetName (hard links, no copy), the list, its signature and the notes.
    const std::wstring staging = root + L"\\salas\\atualizacao-github";
    RemoveTree(staging);
    CreateDirectoryW(staging.c_str(), nullptr);
    std::wstring assets;
    for (const UpdateFile& file : files) {
        const std::wstring from = published + L"\\files\\" + file.path, to = staging + L"\\" + AssetName(file.path);
        if (!CreateHardLinkW(to.c_str(), from.c_str(), nullptr) && !CopyFileW(from.c_str(), to.c_str(), FALSE)) {
            RemoveTree(staging);
            return L"Não deu para preparar " + file.path + L".";
        }
        assets += L" \"" + AssetName(file.path) + L"\"";
    }
    CopyFileW((published + L"\\manifest.txt").c_str(), (staging + L"\\manifest.txt").c_str(), FALSE);
    CopyFileW((published + L"\\manifest.sig").c_str(), (staging + L"\\manifest.sig").c_str(), FALSE);
    const bool history = !beta && CopyFileW((root + L"\\salas\\novidades.txt").c_str(), (staging + L"\\novidades.txt").c_str(), FALSE) != 0;
    if (FILE* note = _wfopen((staging + L"\\notas.txt").c_str(), L"wb")) {
        const std::string text = beta ? notes + "\n\nVersão beta, para teste: só vai para quem marcou Receber versões beta (Emuladores > Arquivos do Ranga).\n" : notes + "\n\nVersão assinada do Ranga Emu. Primeira vez: baixe só o RangaEmu.exe, abra e escolha a pasta; ele baixa o resto. Quem já tem o Ranga não precisa baixar nada daqui: ele se atualiza sozinho.\n";
        std::fwrite(text.data(), 1, text.size(), note);
        std::fclose(note);
    }
    // The tag stays the date (it sorts, and the three newest are kept by it); the title is the number people know
    // ("Ranga 1.5.1"), the date only when the version had no number.
    const std::wstring tag = beta ? L"beta" : L"v" + Wide(version);
    std::string said;
    // One beta at a time, always at the same address.
    if (beta) RunCaptured(L"\"" + gh + L"\" release delete beta --repo " + kGithubRepo + L" --cleanup-tag --yes", L"", said);
    const int code = RunCaptured(L"\"" + gh + L"\" release create " + tag + L" --repo " + kGithubRepo + L" --title \"Ranga " +
                                     Wide(number.empty() ? version : number) +
                                     L"\" --notes-file notas.txt " + (beta ? L"--prerelease --latest=false" : L"--latest") + L" manifest.txt manifest.sig" + (history ? L" novidades.txt" : L"") + assets,
                                 staging, said);
    RemoveTree(staging);
    if (code != 0) {
        if (said.size() > 300) said = said.substr(said.size() - 300);
        return L"O GitHub não aceitou a versão (" + std::to_wstring(code) + L"): " + Wide(said);
    }
    if (beta) return L"";
    // Only the three newest releases stay (each has every file).
    std::string list;
    if (RunCaptured(L"\"" + gh + L"\" release list --repo " + std::wstring(kGithubRepo) + L" --limit 100 --json tagName --jq .[].tagName", L"", list) == 0) {
        std::vector<std::string> tags;
        size_t start = 0;
        while (start < list.size()) {
            size_t end = list.find('\n', start);
            if (end == std::string::npos) end = list.size();
            std::string line = list.substr(start, end - start);
            start = end + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.rfind("v20", 0) == 0) tags.push_back(line);
        }
        std::sort(tags.rbegin(), tags.rend());
        for (size_t i = 3; i < tags.size(); ++i) {
            std::string ignored;
            RunCaptured(L"\"" + gh + L"\" release delete " + Wide(tags[i]) + L" --repo " + kGithubRepo + L" --cleanup-tag --yes", L"", ignored);
        }
    }
    return L"";
}

IntegrityReport CheckIntegrity(const std::wstring& root, const std::function<void(int, int)>& progress) {
    IntegrityReport report;
    report.host = CanPublishUpdate(root);
    std::string manifest, signature;
    if (report.host) {
        // The hosting PC: its own published copy (salas\atualizacao), checked like a friend's would be.
        manifest = ReadBytes(Published(root) + L"\\manifest.txt");
        signature = ReadBytes(Published(root) + L"\\manifest.sig");
        if (manifest.empty()) {
            report.error = L"Nenhuma versão publicada ainda: publique uma para ter com o que comparar.";
            return report;
        }
    } else {
        report.error = FetchManifest(root, manifest, report.repair.source);
        if (!report.error.empty()) return report;
    }
    if (report.host && !Verify(manifest, signature)) {
        report.error = L"A lista dos arquivos não tem a assinatura certa: ignorada.";
        return report;
    }
    std::vector<UpdateFile> files;
    ReadManifest(manifest, report.version, report.repair.notes, files);
    if (report.version.empty() || files.empty()) {
        report.error = L"A lista dos arquivos veio vazia.";
        return report;
    }
    // A beta, or a version the player went back to: the list is another version's, and "repairing" would install it.
    if (!report.host && report.version < InstalledUpdate(root)) {
        report.error = L"Este Ranga é mais novo que a última versão publicada (beta): não há lista desta versão para conferir.";
        return report;
    }
    if (!report.host && report.version <= DeclinedUpdate(root)) {
        report.error = L"Você voltou para uma versão anterior: a conferência só vale para a última versão.";
        return report;
    }
    report.repair.version = report.version;
    report.behind = !report.host && InstalledUpdate(root) != report.version;
    for (size_t i = 0; i < files.size(); ++i) {
        if (progress) progress(static_cast<int>(i), static_cast<int>(files.size()));
        const UpdateFile& file = files[i];
        const std::wstring local = root + L"\\" + file.path;
        ++report.checked;
        if (!Exists(local)) report.missing.push_back(file.path);
        else if (static_cast<uint64_t>(FileSize(local)) != file.size || HashFile(local) != file.sha256) report.different.push_back(file.path);
        else continue;
        report.repair.changed.push_back(file);
        report.repair.download += file.size;
    }
    if (progress) progress(static_cast<int>(files.size()), static_cast<int>(files.size()));
    return report;
}

std::wstring DownloadUpdate(const std::wstring& root, const UpdateInfo& info, const std::function<void(uint64_t, uint64_t)>& progress) {
    const std::wstring source = info.source.empty() ? UpdateSource(root) : info.source, staging = Staging(root);
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
    const std::wstring staging = Staging(root), previous = PreviousFolder(root), older = previous + L".antiga";
    const std::string from = InstalledUpdate(root), fromName = InstalledVersionName(root);
    // A repair of the same version keeps nothing: what it replaces is broken.
    const bool keep = !info.changed.empty() && !from.empty() && from != info.version;
    for (const UpdateFile& file : info.changed)
        if (!Exists(staging + L"\\" + file.path)) return L"Falta o arquivo baixado " + file.path + L".";
    if (keep) {
        RemoveTree(older);
        MoveFileExW(previous.c_str(), older.c_str(), 0);
    }
    const auto savedAt = [&](const std::wstring& path) { return keep ? previous + L"\\" + path : root + L"\\" + path + L".old"; };
    struct Swap { std::wstring path; bool saved; };
    std::vector<Swap> done;
    // Everything back as it was: the new files to the download folder, the old ones to their places.
    const auto undo = [&] {
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            const std::wstring target = root + L"\\" + it->path;
            if (Exists(target)) MoveFileExW(target.c_str(), (staging + L"\\" + it->path).c_str(), MOVEFILE_REPLACE_EXISTING);
            if (it->saved) MoveFileExW(savedAt(it->path).c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        if (keep) {
            RemoveTree(previous);
            MoveFileExW(older.c_str(), previous.c_str(), 0);
        }
    };
    for (const UpdateFile& file : info.changed) {
        const std::wstring target = root + L"\\" + file.path, ready = staging + L"\\" + file.path, saved = savedAt(file.path);
        MakeParents(saved);
        const bool had = Exists(target);
        if (had && !MoveFileExW(target.c_str(), saved.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            undo();
            return L"O arquivo " + file.path + L" está em uso: feche o jogo e tente de novo.";
        }
        done.push_back({file.path, had});
        // A Ranga installed from the exe alone has no cores folder yet.
        if (const size_t slash = target.find_last_of(L'\\'); slash != std::wstring::npos) SHCreateDirectoryExW(nullptr, target.substr(0, slash).c_str(), nullptr);
        if (!MoveFileExW(ready.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            undo();
            return L"Não deu para trocar " + file.path + L".";
        }
    }
    if (keep) {
        std::string list = "versao " + from + "\nnome " + fromName + "\n";
        bool any = false;
        for (const Swap& swap : done) {
            if (!swap.saved) continue;
            const std::wstring saved = savedAt(swap.path);
            list += "arquivo " + HashFile(saved) + " " + std::to_string(FileSize(saved)) + " " + UrlPath(swap.path) + "\n";
            any = true;
        }
        if (any && WriteBytes(previous + L"\\lista.txt", list)) WriteBytes(previous + L"\\confirmar.txt", "0");
        RemoveTree(older);
    }
    EnsureUnicodeIni(Ini(root));
    WritePrivateProfileStringW(L"Ranga", L"versao_atualizacao", Wide(info.version).c_str(), Ini(root).c_str());
    WritePrivateProfileStringW(L"Ranga", L"versao_nome", info.name.empty() ? nullptr : Wide(info.name).c_str(), Ini(root).c_str());
    WritePrivateProfileStringW(L"Ranga", L"versao_pulada", nullptr, Ini(root).c_str());
    return L"";
}

PreviousVersion Previous(const std::wstring& root) {
    PreviousVersion previous;
    std::string notes;
    ReadManifest(ReadBytes(PreviousFolder(root) + L"\\lista.txt"), previous.version, notes, previous.files, &previous.name);
    if (previous.files.empty()) previous.version.clear();
    return previous;
}

std::wstring RollBack(const std::wstring& root) {
    const PreviousVersion previous = Previous(root);
    const std::wstring folder = PreviousFolder(root);
    if (previous.version.empty()) return L"Não há versão anterior guardada.";
    for (const UpdateFile& file : previous.files)
        if (HashFile(folder + L"\\" + file.path) != file.sha256) return L"A versão anterior guardada está incompleta (" + file.path + L"): nada foi trocado.";
    struct Swap { std::wstring path; bool had; };
    std::vector<Swap> done;
    std::wstring error;
    for (const UpdateFile& file : previous.files) {
        const std::wstring target = root + L"\\" + file.path, saved = folder + L"\\" + file.path;
        const bool had = Exists(target);
        if (had && !MoveFileExW(target.c_str(), (target + L".old").c_str(), MOVEFILE_REPLACE_EXISTING)) {
            error = L"O arquivo " + file.path + L" está em uso: feche o jogo e tente de novo.";
            break;
        }
        done.push_back({file.path, had});
        if (!MoveFileExW(saved.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            error = L"Não deu para trocar " + file.path + L".";
            break;
        }
    }
    if (!error.empty()) {
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            const std::wstring target = root + L"\\" + it->path;
            if (Exists(target)) MoveFileExW(target.c_str(), (folder + L"\\" + it->path).c_str(), MOVEFILE_REPLACE_EXISTING);
            if (it->had) MoveFileExW((target + L".old").c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        return error;
    }
    const std::string left = InstalledUpdate(root);
    EnsureUnicodeIni(Ini(root));
    WritePrivateProfileStringW(L"Ranga", L"versao_pulada", Wide(left).c_str(), Ini(root).c_str());
    WritePrivateProfileStringW(L"Ranga", L"versao_atualizacao", Wide(previous.version).c_str(), Ini(root).c_str());
    WritePrivateProfileStringW(L"Ranga", L"versao_nome", previous.name.empty() ? nullptr : Wide(previous.name).c_str(), Ini(root).c_str());
    RemoveTree(folder);
    return L"";
}

std::string DeclinedUpdate(const std::wstring& root) { return Utf8(ReadIni(Ini(root), L"Ranga", L"versao_pulada", L"")); }

void ForgetDeclined(const std::wstring& root) { WritePrivateProfileStringW(L"Ranga", L"versao_pulada", nullptr, Ini(root).c_str()); }

bool UpdateNeverOpened(const std::wstring& root) {
    const std::wstring marker = PreviousFolder(root) + L"\\confirmar.txt";
    if (!Exists(marker) || Previous(root).version.empty()) return false;
    const bool before = std::atoi(ReadBytes(marker).c_str()) > 0;
    if (!before) WriteBytes(marker, "1");
    return before;
}

void ConfirmUpdate(const std::wstring& root) { DeleteFileW((PreviousFolder(root) + L"\\confirmar.txt").c_str()); }

bool BetaChannel(const std::wstring& root) { return ReadIni(Ini(root), L"Ranga", L"beta", L"") == L"1"; }

void SetBetaChannel(const std::wstring& root, bool on) {
    EnsureUnicodeIni(Ini(root));
    WritePrivateProfileStringW(L"Ranga", L"beta", on ? L"1" : nullptr, Ini(root).c_str());
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
    const std::wstring older = PreviousFolder(root) + L".antiga";
    if (IsDirectory(older)) RemoveTree(older);
}

int DownloadUrl(const std::wstring& url, std::string* body, const std::wstring& toFile, const std::function<void(uint64_t)>& got) {
    return HttpGet(url, L"", body, toFile, got);
}

bool SignedByRanga(const std::string& message, const std::string& signature) { return Verify(message, signature); }

std::wstring SignWithRangaKey(const std::wstring& root, const std::string& message, std::string& signature) {
    std::wstring keyError;
    const std::string key = LoadSigningKey(root, keyError);
    if (key.empty()) return keyError;
    if (!Sign(key, message, signature)) return L"A assinatura falhou (a chave está estragada?).";
    return L"";
}

std::wstring SealSigningKey(const std::wstring& root) {
    const std::string plain = ReadBytes(PrivateKey(root));
    if (plain.empty()) return L"Não há salas\\chave-atualizacao.bin para cifrar.";
    // A key that does not sign what the public half checks is not Ranga's: nothing is sealed.
    std::string probe;
    if (!Sign(plain, "ranga-teste-da-chave", probe) || !Verify("ranga-teste-da-chave", probe)) return L"Essa chave não é a do Ranga: nada foi cifrado.";
    const std::wstring first = AskPassword(L"Escolha a senha da chave (12 ou mais caracteres). Guarde no 1Password: sem ela não dá para publicar.");
    if (first.empty()) return L"Cancelado.";
    if (first.size() < 12) return L"Senha curta demais: use 12 ou mais caracteres. Nada foi cifrado.";
    if (AskPassword(L"Digite a mesma senha de novo.") != first) return L"As duas senhas não batem. Nada foi cifrado.";
    const std::string sealed = Seal(plain, first);
    // Read back and checked before it counts: the sealed file opens with the password and signs as the key does.
    std::string again, signature;
    if (sealed.empty() || (again = Unseal(sealed, first)) != plain || !Sign(again, "ranga-teste-da-chave", signature) ||
        !Verify("ranga-teste-da-chave", signature))
        return L"A chave cifrada não conferiu. Nada foi gravado.";
    if (!WriteBytes(SealedKey(root), sealed) || ReadBytes(SealedKey(root)) != sealed) return L"Não deu para gravar salas\\chave-atualizacao.cifrada.";
    return L"";
}

std::string Sha256OfFile(const std::wstring& path) { return HashFile(path); }

int RunHiddenCommand(const std::wstring& command, const std::wstring& folder, std::string& out) { return RunCaptured(command, folder, out); }

void RemoveFolderTree(const std::wstring& folder) { RemoveTree(folder); }

}
