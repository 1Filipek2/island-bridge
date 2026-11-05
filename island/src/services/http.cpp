#include "services/http.h"

namespace island
{
std::string http_get(const wchar_t* host, const wchar_t* path, bool https)
{
    std::string response;
    HINTERNET h_session = WinHttpOpen(L"DynamicIsland/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h_session)
    {
        host::log(L"Weather HttpGet: WinHttpOpen failed with error %lu", GetLastError());
        return response;
    }

    HINTERNET h_connect =
        WinHttpConnect(h_session, host, https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
    if (h_connect)
    {
        HINTERNET h_request = WinHttpOpenRequest(h_connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
        if (h_request)
        {
            if (WinHttpSendRequest(h_request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(h_request, nullptr))
            {
                DWORD size = 0;
                DWORD downloaded = 0;
                do
                {
                    if (WinHttpQueryDataAvailable(h_request, &size) && size > 0)
                    {
                        std::vector<char> buffer(size + 1);
                        if (WinHttpReadData(h_request, buffer.data(), size, &downloaded))
                        {
                            buffer[downloaded] = '\0';
                            response.append(buffer.data());
                        }
                        else
                        {
                            host::log(L"Weather HttpGet: WinHttpReadData failed with error %lu", GetLastError());
                        }
                    }
                } while (size > 0);
            }
            else
            {
                host::log(L"Weather HttpGet: WinHttpSendRequest/ReceiveResponse failed with error %lu", GetLastError());
            }
            WinHttpCloseHandle(h_request);
        }
        else
        {
            host::log(L"Weather HttpGet: WinHttpOpenRequest failed with error %lu", GetLastError());
        }
        WinHttpCloseHandle(h_connect);
    }
    else
    {
        host::log(L"Weather HttpGet: WinHttpConnect failed with error %lu", GetLastError());
    }
    WinHttpCloseHandle(h_session);
    return response;
}

HttpResult http_get_ex(const wchar_t* host, const wchar_t* path, bool https, const wchar_t* user_agent)
{
    HttpResult result;
    HINTERNET h_session =
        WinHttpOpen(user_agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h_session)
    {
        return result;
    }
    // resolve, connect, send, receive (ms). Without these a dead network can park
    // the lyrics thread for a very long time.
    WinHttpSetTimeouts(h_session, 8000, 8000, 10000, 15000);

    HINTERNET h_connect =
        WinHttpConnect(h_session, host, https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
    if (h_connect)
    {
        HINTERNET h_request = WinHttpOpenRequest(h_connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
        if (h_request)
        {
            if (WinHttpSendRequest(h_request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(h_request, nullptr))
            {
                DWORD status = 0;
                DWORD status_size = sizeof(status);
                if (WinHttpQueryHeaders(h_request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX))
                {
                    bool read_ok = true;
                    for (;;)
                    {
                        DWORD size = 0;
                        if (!WinHttpQueryDataAvailable(h_request, &size))
                        {
                            read_ok = false;
                            break;
                        }
                        if (size == 0)
                        {
                            break;
                        }
                        std::vector<char> buffer(size);
                        DWORD downloaded = 0;
                        if (!WinHttpReadData(h_request, buffer.data(), size, &downloaded))
                        {
                            read_ok = false;
                            break;
                        }
                        result.body.append(buffer.data(), downloaded);
                        if (result.body.size() > 4u * 1024u * 1024u)
                        {
                            read_ok = false;
                            break;
                        }
                    }
                    if (read_ok)
                    {
                        result.ok = true;
                        result.status = status;
                    }
                }
            }
            WinHttpCloseHandle(h_request);
        }
        WinHttpCloseHandle(h_connect);
    }
    WinHttpCloseHandle(h_session);
    return result;
}

std::wstring percent_encode_utf8(const std::wstring& text)
{
    // An already encoded "%XX" value is passed through untouched.
    auto looks_pre_encoded = [](const std::wstring& value)
    {
        const size_t pos = value.find(L'%');
        if (pos == std::wstring::npos || pos + 2 >= value.size())
        {
            return false;
        }
        return iswxdigit(value[pos + 1]) && iswxdigit(value[pos + 2]);
    };
    if (looks_pre_encoded(text))
    {
        return text;
    }

    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1)
    {
        return std::wstring();
    }

    std::string utf8(static_cast<size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8.data(), bytes, nullptr, nullptr);

    static constexpr wchar_t hex_digits[] = L"0123456789ABCDEF";
    std::wstring out;
    out.reserve(utf8.size() * 3);
    for (const unsigned char c : utf8)
    {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved)
        {
            out.push_back(static_cast<wchar_t>(c));
        }
        else
        {
            out.push_back(L'%');
            out.push_back(hex_digits[(c >> 4) & 0x0f]);
            out.push_back(hex_digits[c & 0x0f]);
        }
    }
    return out;
}

}
