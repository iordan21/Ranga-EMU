#include "CrashReport.hpp"

#include "Common.hpp"
#include "Http.hpp"

#include <bcrypt.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace Ranga::CrashReport {

namespace {

// The Ranga's project in Sentry (organization nadroi, project ranga-emu, data in the EU; made on 26/11/2026). Public by
// Sentry's design (it only lets one send events to this project), so it can live in the program. Empty: the button
// does not show.
constexpr const char* kDsn = "https://5994c22c7a4d3161104ed8a3cf765999@o4512496258711552.ingest.de.sentry.io/4512496268214352";

// Each log goes as its last part only: what matters is how the run ended.
constexpr size_t kLogLimit = 256 * 1024;

std::string Env(const wchar_t* name) {
    wchar_t value[512];
    const DWORD size = GetEnvironmentVariableW(name, value, 512);
    return size > 0 && size < 512 ? Utf8(value) : std::string();
}

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// Every place of needle (ASCII letters in any case) replaced by with.
void ReplaceAll(std::string& text, const std::string& needle, const std::string& with) {
    if (needle.empty()) return;
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        bool match = i + needle.size() <= text.size();
        for (size_t k = 0; match && k < needle.size(); ++k) match = Lower(text[i + k]) == Lower(needle[k]);
        if (match) {
            out += with;
            i += needle.size();
        } else {
            out += text[i++];
        }
    }
    text.swap(out);
}

bool EmailLocal(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-'; }
bool EmailDomain(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-'; }

void CleanEmails(std::string& text) {
    std::string out;
    size_t copied = 0;
    for (size_t at = text.find('@'); at != std::string::npos; at = text.find('@', at + 1)) {
        size_t start = at;
        while (start > copied && EmailLocal(text[start - 1])) --start;
        size_t end = at + 1;
        while (end < text.size() && EmailDomain(text[end])) ++end;
        while (end > at + 1 && text[end - 1] == '.') --end; // the sentence's full stop is not the domain's
        const std::string domain = text.substr(at + 1, end - at - 1);
        if (start == at || domain.find('.') == std::string::npos) continue;
        out.append(text, copied, start - copied);
        out += "<email>";
        copied = end;
        at = end - 1;
    }
    out.append(text, copied, std::string::npos);
    text.swap(out);
}

// a.b.c.d with each part 0-255 and nothing number-like around it. 127.0.0.1 and 0.0.0.0 stay: they say nothing.
void CleanAddresses(std::string& text) {
    std::string out;
    size_t copied = 0;
    size_t i = 0;
    while (i < text.size()) {
        if (!std::isdigit(static_cast<unsigned char>(text[i])) || (i > 0 && (std::isdigit(static_cast<unsigned char>(text[i - 1])) || text[i - 1] == '.'))) {
            ++i;
            continue;
        }
        size_t j = i;
        int parts = 0;
        bool good = true;
        while (parts < 4) {
            size_t digits = 0;
            int value = 0;
            while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j])) && digits < 4) {
                value = value * 10 + (text[j] - '0');
                ++j;
                ++digits;
            }
            if (digits == 0 || digits > 3 || value > 255) {
                good = false;
                break;
            }
            ++parts;
            if (parts < 4) {
                if (j < text.size() && text[j] == '.') ++j;
                else {
                    good = false;
                    break;
                }
            }
        }
        if (good && j < text.size() && (std::isdigit(static_cast<unsigned char>(text[j])) || (text[j] == '.' && j + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[j + 1])))))
            good = false;
        if (!good) {
            while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) || text[i] == '.')) ++i;
            continue;
        }
        const std::string address = text.substr(i, j - i);
        if (address != "127.0.0.1" && address != "0.0.0.0") {
            out.append(text, copied, i - copied);
            out += "<ip>";
            copied = j;
        }
        i = j;
    }
    out.append(text, copied, std::string::npos);
    text.swap(out);
}

// "token=abc", "senha: abc", "Authorization: Bearer abc": the rest of the line after the separator goes.
void CleanSecrets(std::string& text) {
    static const char* const kKeys[] = {"token", "senha", "password", "passwd", "secret", "segredo", "authorization", "apikey", "api_key", "api-key", "bearer", "cookie"};
    std::string out;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        std::string lower = line;
        for (char& c : lower) c = Lower(c);
        size_t cut = std::string::npos;
        for (const char* key : kKeys) {
            for (size_t at = lower.find(key); at != std::string::npos && cut == std::string::npos; at = lower.find(key, at + 1)) {
                size_t k = at + std::string(key).size();
                while (k < lower.size() && (std::isalnum(static_cast<unsigned char>(lower[k])) || lower[k] == '_')) ++k;
                while (k < lower.size() && (lower[k] == ' ' || lower[k] == '"' || lower[k] == '\'')) ++k;
                if (k < lower.size() && (lower[k] == '=' || lower[k] == ':')) cut = k + 1;
            }
        }
        if (cut != std::string::npos) {
            const bool carriage = !line.empty() && line.back() == '\r';
            line = line.substr(0, cut) + " <segredo>" + (carriage ? "\r" : "");
        }
        out += line;
        if (end < text.size()) out += '\n';
        start = end + 1;
    }
    text.swap(out);
}

std::string Json(const std::string& text) {
    std::string out = "\"";
    for (const unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char hex[8];
                std::snprintf(hex, sizeof(hex), "\\u%04x", c);
                out += hex;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out + "\"";
}

std::string Hex(unsigned long code) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08lX", code);
    return text;
}

std::string NewEventId() {
    unsigned char bytes[16] = {};
    BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::string id;
    char two[3];
    for (const unsigned char b : bytes) {
        std::snprintf(two, sizeof(two), "%02x", b);
        id += two;
    }
    return id;
}

// https://KEY@HOST/PROJECT -> the envelope's address and the key.
bool ParseDsn(const std::string& dsn, std::string& url, std::string& key) {
    const size_t scheme = dsn.find("://");
    const size_t at = dsn.find('@');
    const size_t slash = dsn.rfind('/');
    if (scheme == std::string::npos || at == std::string::npos || slash == std::string::npos || at < scheme || slash < at) return false;
    key = dsn.substr(scheme + 3, at - scheme - 3);
    if (const size_t colon = key.find(':'); colon != std::string::npos) key.resize(colon);
    const std::string project = dsn.substr(slash + 1);
    if (key.empty() || project.empty()) return false;
    url = dsn.substr(0, scheme + 3) + dsn.substr(at + 1, slash - at - 1) + "/api/" + project + "/envelope/";
    return true;
}

}

std::string Dsn() {
    const std::string test = Env(L"RANGA_SENTRY_DSN");
    return test.empty() ? kDsn : test;
}

bool Enabled() { return !kRangaAlpha && !Dsn().empty(); }

std::string Clean(const std::string& text) {
    std::string out = text;
    const std::string profile = Env(L"USERPROFILE");
    if (profile.size() > 3) {
        ReplaceAll(out, profile, "C:\\Users\\<usuario>");
        std::string forward = profile;
        for (char& c : forward)
            if (c == '\\') c = '/';
        ReplaceAll(out, forward, "C:/Users/<usuario>");
    }
    // The PC's name first: it often has the user's name inside ("PC-DO-FULANO"). A name of one or two letters would
    // eat pieces of every word.
    if (const std::string pc = Env(L"COMPUTERNAME"); pc.size() >= 3) ReplaceAll(out, pc, "<pc>");
    if (const std::string user = Env(L"USERNAME"); user.size() >= 3) ReplaceAll(out, user, "<usuario>");
    CleanEmails(out);
    CleanAddresses(out);
    CleanSecrets(out);
    return out;
}

std::string CrashPlace(const std::string& log) {
    const size_t line = log.rfind("[queda] codigo=");
    if (line == std::string::npos) return "";
    const size_t at = log.find(" em ", line);
    const size_t lineEnd = log.find_first_of("\r\n", line);
    if (at == std::string::npos || (lineEnd != std::string::npos && at > lineEnd)) return "";
    const size_t start = at + 4;
    size_t end = log.find_first_of(" \r\n", start);
    if (end == std::string::npos) end = log.size();
    std::string place = log.substr(start, end - start);
    // A file name and an offset, nothing else (a path would carry the user's folder).
    if (place.size() > 160 || place.find_first_of("\\/:") != std::string::npos) return "";
    return place;
}

std::string Envelope(const Report& report, const std::string& dsn, const std::string& eventId) {
    const std::string code = Hex(report.code);
    // "pcsx2_libretro.dll" from "pcsx2_libretro.dll+0x1A2B3C"; "codigo-gerado" for code a core made while running.
    std::string module = report.where.substr(0, report.where.find_first_of("+@"));
    const std::string title = report.emulator + " fechou com erro " + code + (module.empty() ? "" : " em " + module);
    std::string event = "{\"event_id\":" + Json(eventId) + ",\"timestamp\":" + std::to_string(static_cast<long long>(std::time(nullptr))) +
                        ",\"platform\":\"native\",\"level\":\"fatal\",\"logger\":\"ranga\",\"environment\":\"producao\"";
    if (!report.version.empty()) event += ",\"release\":" + Json("ranga-emu@" + report.version);
    if (!report.update.empty()) event += ",\"dist\":" + Json(report.update);
    event += ",\"message\":{\"formatted\":" + Json(title + " (" + report.console + ")") + "}";
    event += ",\"exception\":{\"values\":[{\"type\":" + Json(code) + ",\"value\":" +
             Json(title + " depois de " + std::to_string(report.seconds) + " s" + (report.where.empty() ? "" : " (" + report.where + ")")) + ",\"mechanism\":{\"type\":\"exit_code\",\"handled\":false}}]}";
    event += ",\"tags\":{\"console\":" + Json(report.console) + ",\"emulador\":" + Json(report.emulator) + ",\"codigo\":" + Json(code);
    if (!report.game.empty()) event += ",\"jogo\":" + Json(report.game);
    if (!report.serial.empty()) event += ",\"serial\":" + Json(report.serial);
    if (!module.empty()) event += ",\"modulo\":" + Json(module);
    event += "},\"extra\":{\"segundos\":" + std::to_string(report.seconds);
    if (!report.where.empty()) event += ",\"onde\":" + Json(report.where);
    event += "}";
    // Grouped by emulator, code and the file it crashed in: "Eden 0xC0000005 em eden_libretro.dll" is one problem,
    // whatever the game.
    event += ",\"fingerprint\":[" + Json(report.emulator) + "," + Json(code) + "," + Json(module.empty() ? "?" : module) + "]";
    // Nobody's address: Sentry would otherwise keep the sender's IP.
    event += ",\"user\":{\"ip_address\":null}}";

    char sent[32];
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_s(&utc, &now);
    std::strftime(sent, sizeof(sent), "%Y-%m-%dT%H:%M:%SZ", &utc);
    std::string body = "{\"event_id\":" + Json(eventId) + ",\"dsn\":" + Json(dsn) + ",\"sent_at\":" + Json(sent) + "}\n";
    body += "{\"type\":\"event\",\"length\":" + std::to_string(event.size()) + ",\"content_type\":\"application/json\"}\n" + event + "\n";
    for (const auto& [name, text] : report.logs) {
        std::string tail = text.size() > kLogLimit ? text.substr(text.size() - kLogLimit) : text;
        tail = Clean(tail);
        body += "{\"type\":\"attachment\",\"length\":" + std::to_string(tail.size()) + ",\"filename\":" + Json(name) +
                ",\"content_type\":\"text/plain\"}\n" + tail + "\n";
    }
    return body;
}

std::wstring Send(const Report& report) {
    if (kRangaAlpha) return L"O Ranga Alpha não envia nada.";
    const std::string dsn = Dsn();
    std::string url, key;
    if (dsn.empty() || !ParseDsn(dsn, url, key)) return L"O envio ainda não está ligado nesta versão do Ranga.";
    const std::string eventId = NewEventId();
    const HttpResult answer = HttpRequest("POST", url, Envelope(report, dsn, eventId), "application/x-sentry-envelope", "RangaEmu", 30000,
                                          "X-Sentry-Auth: Sentry sentry_version=7, sentry_client=ranga-emu/1.0, sentry_key=" + key + "\r\n");
    if (answer.status == 200) return L"";
    if (answer.status == 0) return L"Sem resposta (sem internet?). O relatório não foi enviado.";
    if (answer.status == 429) return L"O Ranga recebeu relatórios demais agora. Tente mais tarde.";
    return L"O envio falhou (resposta " + std::to_wstring(answer.status) + L").";
}

}
