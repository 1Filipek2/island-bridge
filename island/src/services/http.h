#pragma once

#include "core/common.h"

namespace island
{
std::string http_get(const wchar_t* host, const wchar_t* path, bool https = true);

// Separates "the server answered" from "the transport failed", which http_get cannot.
struct HttpResult
{
    bool ok = false;  // headers received AND the body was read to the end
    DWORD status = 0; // HTTP status, valid only when ok
    std::string body;

    // 200 with a body or 404; anything else is a failure that must never be cached.
    bool is_definitive() const
    {
        return ok && ((status == 200 && !body.empty()) || status == 404);
    }
};

HttpResult http_get_ex(const wchar_t* host, const wchar_t* path, bool https, const wchar_t* user_agent);
std::wstring percent_encode_utf8(const std::wstring& text);

}
