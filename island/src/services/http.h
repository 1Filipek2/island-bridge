#pragma once

#include "core/common.h"
namespace island
{

std::string HttpGet(const wchar_t* host, const wchar_t* path, bool https = true);

// HTTP GET that distinguishes "the server answered" from "the transport failed".
// HttpGet() above returns an empty string for both, which is why a lyrics cache
// cannot be built on it: a dropped connection would look like "no lyrics".
struct HttpResult {
    bool ok = false;     // headers received AND the body was read to the end
    DWORD status = 0;    // HTTP status, valid only when ok
    std::string body;

    // A real answer: lyrics (200 with a body) or "no such track" (404). Anything
    // else (timeout, 429, 5xx, empty 200) is a failure that must never be cached.
    bool IsDefinitive() const {
        return ok && ((status == 200 && !body.empty()) || status == 404);
    }
};
HttpResult HttpGetEx(const wchar_t* host, const wchar_t* path, bool https, const wchar_t* userAgent);
std::wstring PercentEncodeUtf8(const std::wstring& text);

}
