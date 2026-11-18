#ifndef LAUNCHER_HTTP_HPP
#define LAUNCHER_HTTP_HPP

// One HTTP or HTTPS request through WinHTTP, for the profiles on the hosting PC's room and for RetroAchievements.
// It blocks: run it off any thread that draws. Self-contained, so Ranga Retro builds it too.

#include <string>

namespace Ranga {

struct HttpResult {
    int status = 0;      // 0: no answer at all (no network, wrong address, timeout)
    std::string body;
};

// url is "http(s)://host[:port]/path", or "host:port" plus a path starting with "/" (Ranga's room, plain HTTP).
// body empty and method GET for a plain download; contentType goes with a body.
HttpResult HttpRequest(const std::string& method, const std::string& url, const std::string& body = "",
                       const std::string& contentType = "", const std::string& userAgent = "RangaEmu", int timeoutMs = 15000,
                       const std::string& headers = "");

// Percent-encoding for a query string or a form field.
std::string UrlEncode(const std::string& text);

}

#endif
