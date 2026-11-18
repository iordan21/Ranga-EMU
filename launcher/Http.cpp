#include "Http.hpp"

#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <winhttp.h>

#include <vector>

namespace Ranga {

namespace {

std::wstring Widen(const std::string& text) {
    if (text.empty()) return L"";
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

}

std::string UrlEncode(const std::string& text) {
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += digits[c >> 4];
            out += digits[c & 15];
        }
    }
    return out;
}

HttpResult HttpRequest(const std::string& method, const std::string& url, const std::string& body, const std::string& contentType,
                       const std::string& userAgent, int timeoutMs) {
    HttpResult result;
    std::string full = url;
    if (full.rfind("http://", 0) != 0 && full.rfind("https://", 0) != 0) full = "http://" + full;
    std::wstring wide = Widen(full);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[4096] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 4096;
    wchar_t extra[4096] = {};
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) return result;
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    const std::wstring target = std::wstring(path) + extra;

    HINTERNET session = WinHttpOpen(Widen(userAgent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) return result;
    WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    if (HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0)) {
        if (HINTERNET request = WinHttpOpenRequest(connection, Widen(method).c_str(), target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)) {
            const std::wstring headers = contentType.empty() ? L"" : L"Content-Type: " + Widen(contentType) + L"\r\n";
            if (WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), static_cast<DWORD>(-1L),
                                   body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                                   static_cast<DWORD>(body.size()), 0) &&
                WinHttpReceiveResponse(request, nullptr)) {
                DWORD code = 0, size = sizeof(code);
                WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
                std::vector<char> block(1 << 16);
                bool complete = true;
                for (;;) {
                    DWORD read = 0;
                    if (!WinHttpReadData(request, block.data(), static_cast<DWORD>(block.size()), &read)) { complete = false; break; }
                    if (read == 0) break;
                    result.body.append(block.data(), read);
                    // Nothing Ranga asks for here is big; a runaway answer stops.
                    if (result.body.size() > (64u << 20)) { complete = false; break; }
                }
                result.status = complete ? static_cast<int>(code) : 0;
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connection);
    }
    WinHttpCloseHandle(session);
    if (result.status == 0) result.body.clear();
    return result;
}

}
