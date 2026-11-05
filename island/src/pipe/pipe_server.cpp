#include "pipe/pipe_server.h"
#include "ui/icons.h"
#include "core/model.h"
#include "notifications/notification_center.h"
#include "pipe/pipe_protocol.h"
#include "core/pixels.h"
#include "core/state.h"
#include "core/util.h"

namespace island
{
namespace island_pipe
{
namespace
{

std::wstring process_image_path(DWORD process_id)
{
    OwnedHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id));

    if (!process.valid())
    {
        return std::wstring{};
    }

    std::wstring path(MAX_PATH, L'\0');

    for (int attempt = 0; attempt < 4; ++attempt)
    {
        DWORD length = static_cast<DWORD>(path.size());

        if (QueryFullProcessImageNameW(process.get(), 0, path.data(), &length))
        {
            path.resize(length);
            return path;
        }

        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        {
            break;
        }

        path.resize(path.size() * 2);
    }

    return std::wstring{};
}

std::wstring expected_client_path()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);

    if (length == 0 || length >= MAX_PATH)
    {
        return std::wstring{};
    }

    std::wstring result(path, length);
    const std::size_t slash = result.find_last_of(L'\\');

    if (slash == std::wstring::npos)
    {
        return std::wstring{};
    }

    result.resize(slash + 1);
    result += client_file_name;
    return result;
}

bool is_trusted_client(HANDLE pipe)
{
    ULONG process_id = 0;

    if (!GetNamedPipeClientProcessId(pipe, &process_id) || process_id == 0)
    {
        return false;
    }

    const std::wstring actual = process_image_path(process_id);
    const std::wstring expected = expected_client_path();

    if (actual.empty() || expected.empty())
    {
        return false;
    }

    return CompareStringOrdinal(actual.c_str(), static_cast<int>(actual.size()), expected.c_str(),
                                static_cast<int>(expected.size()), TRUE) == CSTR_EQUAL;
}

std::wstring current_user_sid()
{
    HANDLE raw_token = nullptr;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token))
    {
        return std::wstring{};
    }

    OwnedHandle token(raw_token);
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);

    if (size == 0)
    {
        return std::wstring{};
    }

    std::vector<std::uint8_t> buffer(size);

    if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size))
    {
        return std::wstring{};
    }

    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR sid_text = nullptr;

    if (!ConvertSidToStringSidW(user->User.Sid, &sid_text))
    {
        return std::wstring{};
    }

    std::wstring result(sid_text);
    LocalFree(sid_text);
    return result;
}

HANDLE create_pipe()
{
    const std::wstring sid = current_user_sid();

    if (sid.empty())
    {
        return INVALID_HANDLE_VALUE;
    }

    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + sid + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
    {
        return INVALID_HANDLE_VALUE;
    }

    SECURITY_ATTRIBUTES attributes = {};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;

    const HANDLE pipe =
        CreateNamedPipeW(pipe_name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                         PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0,
                         max_message_bytes, 0, &attributes);

    LocalFree(descriptor);
    return pipe;
}

enum class WaitResult
{
    Completed,
    Stopped,
    Failed
};

WaitResult wait_for(HANDLE pipe, OVERLAPPED& overlapped, DWORD& transferred)
{
    const HANDLE waits[2] = {g_stop_event, overlapped.hEvent};
    const DWORD signaled = WaitForMultipleObjects(2, waits, FALSE, INFINITE);

    if (signaled == WAIT_OBJECT_0)
    {
        CancelIoEx(pipe, &overlapped);
        GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
        return WaitResult::Stopped;
    }

    if (signaled != WAIT_OBJECT_0 + 1)
    {
        return WaitResult::Failed;
    }

    return GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) ? WaitResult::Completed : WaitResult::Failed;
}

WaitResult connect_client(HANDLE pipe, OVERLAPPED& overlapped)
{
    ResetEvent(overlapped.hEvent);

    if (ConnectNamedPipe(pipe, &overlapped))
    {
        return WaitResult::Completed;
    }

    const DWORD error = GetLastError();

    if (error == ERROR_PIPE_CONNECTED)
    {
        return WaitResult::Completed;
    }

    if (error != ERROR_IO_PENDING)
    {
        return WaitResult::Failed;
    }

    DWORD transferred = 0;
    return wait_for(pipe, overlapped, transferred);
}

struct Prepared
{
    std::uint8_t category = 0;
    std::wstring key;
    std::wstring app;
    std::wstring line;
    PixelsRef icon;
};

Prepared prepare(const PipeMessage& message)
{
    Prepared prepared;
    prepared.category = message.category;
    prepared.app = sanitize(message.app.empty() ? message.bundle : message.app, max_app_chars);
    prepared.key = sanitize(message.bundle.empty() ? message.app : message.bundle, max_key_chars);

    std::wstring heading = sanitize(message.title, max_title_chars);
    const std::wstring subtitle = sanitize(message.subtitle, max_title_chars);
    const std::wstring body = sanitize(message.body, max_body_chars);

    if (!subtitle.empty())
    {
        heading = heading.empty() ? subtitle : heading + L" \u00B7 " + subtitle;
    }

    if (heading.empty())
    {
        heading = prepared.app.empty() ? std::wstring(L"iPhone") : prepared.app;
    }

    if (!body.empty())
    {
        heading += L" - " + body;
    }

    prepared.line = sanitize(heading, max_title_chars);
    prepared.icon = decode_icon(message.icon);

    if (!prepared.icon && !prepared.app.empty())
    {
        prepared.icon = find_app_icon_by_name(prepared.app, 64);
    }

    if (prepared.app.empty())
    {
        prepared.app = L"iPhone";
    }

    return prepared;
}

std::wstring local_time_label()
{
    wchar_t buffer[32] = {};
    const int written = GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, buffer,
                                        static_cast<int>(std::size(buffer)));
    return written > 1 ? std::wstring(buffer, static_cast<std::size_t>(written - 1)) : std::wstring{};
}

void remember(const Prepared& prepared)
{
    if (!get_settings_copy().notification_center_module)
    {
        return;
    }

    notification_center::Incoming incoming{prepared.key, prepared.app, prepared.line, local_time_label(),
                                           prepared.icon};

    {
        std::lock_guard lock(g_state_mutex);
        auto next = g_state.notification_center
                        ? std::make_shared<notification_center::History>(*g_state.notification_center)
                        : std::make_shared<notification_center::History>();
        next->record(std::move(incoming));
        g_state.notification_center = std::move(next);
    }

    g_notification_center_unread = true;
    g_layout_dirty = true;
    trigger_nudge();
}

bool deliver(const Prepared& prepared)
{
    const Settings settings = get_settings_copy();

    if (settings.notification_respect_dn_d && g_is_dn_d_active.load())
    {
        host::log(L"Island pipe: skipped, Do Not Disturb active");
        return false;
    }

    NotificationSnapshot snapshot;
    snapshot.active = true;
    snapshot.app = prepared.app;
    snapshot.title = prepared.line;
    snapshot.icon = prepared.icon;
    snapshot.expires_at = now_seconds() + display_seconds;

    {
        std::lock_guard lock(g_state_mutex);
        g_state.notification = std::move(snapshot);
    }

    host::log(L"Island pipe: notification shown (category %u)", static_cast<unsigned>(prepared.category));
    trigger_nudge();
    return true;
}

class DisplayQueue
{
public:
    void push(Prepared message)
    {
        if (pending_.size() >= max_queued)
        {
            pending_.pop_front();
        }

        pending_.push_back(std::move(message));
        pump();
    }

    void pump()
    {
        while (!pending_.empty() && now_seconds() >= busy_until_)
        {
            Prepared next = std::move(pending_.front());
            pending_.pop_front();

            if (deliver(next))
            {
                busy_until_ = now_seconds() + display_seconds + display_gap_seconds;
            }
        }
    }

    DWORD timeout_ms() const
    {
        if (pending_.empty())
        {
            return INFINITE;
        }

        const double remaining = busy_until_ - now_seconds();
        return remaining <= 0.0 ? 0 : static_cast<DWORD>(remaining * 1000.0) + 1;
    }

    std::size_t size() const
    {
        return pending_.size();
    }

private:
    std::deque<Prepared> pending_;
    double busy_until_ = 0.0;
};

WaitResult wait_with_queue(HANDLE pipe, OVERLAPPED& overlapped, DWORD& transferred, DisplayQueue& queue)
{
    const HANDLE waits[2] = {g_stop_event, overlapped.hEvent};

    while (true)
    {
        const DWORD signaled = WaitForMultipleObjects(2, waits, FALSE, queue.timeout_ms());

        if (signaled == WAIT_TIMEOUT)
        {
            queue.pump();
            continue;
        }

        if (signaled == WAIT_OBJECT_0)
        {
            CancelIoEx(pipe, &overlapped);
            GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
            return WaitResult::Stopped;
        }

        if (signaled != WAIT_OBJECT_0 + 1)
        {
            return WaitResult::Failed;
        }

        return GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) ? WaitResult::Completed : WaitResult::Failed;
    }
}

void serve_client(HANDLE pipe, OVERLAPPED& overlapped)
{
    std::array<std::uint8_t, max_message_bytes> buffer = {};
    RateLimiter limiter;
    DisplayQueue queue;

    while (WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT)
    {
        ResetEvent(overlapped.hEvent);
        DWORD transferred = 0;
        bool completed =
            ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &transferred, &overlapped) != FALSE;

        if (!completed)
        {
            const DWORD error = GetLastError();

            if (error == ERROR_MORE_DATA)
            {
                host::log(L"Island pipe: message over %u bytes, disconnecting client", max_message_bytes);
                return;
            }

            if (error != ERROR_IO_PENDING)
            {
                return;
            }

            const WaitResult result = wait_with_queue(pipe, overlapped, transferred, queue);

            if (result != WaitResult::Completed)
            {
                if (result == WaitResult::Failed && GetLastError() == ERROR_MORE_DATA)
                {
                    host::log(L"Island pipe: message over %u bytes, disconnecting client", max_message_bytes);
                }

                return;
            }
        }

        if (!limiter.allow(GetTickCount64()))
        {
            continue;
        }

        const std::optional<PipeMessage> message = parse_message(buffer.data(), transferred);

        if (!message)
        {
            host::log(L"Island pipe: invalid message (%u bytes) dropped", static_cast<unsigned>(transferred));
            continue;
        }

        Prepared prepared = prepare(*message);
        remember(prepared);
        queue.push(std::move(prepared));
    }
}

}
}

namespace island_pipe
{

DWORD WINAPI thread_proc(void*)
{
    OwnedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));

    if (!event.valid())
    {
        return 0;
    }

    while (WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT)
    {
        OwnedHandle pipe(create_pipe());

        if (!pipe.valid())
        {
            host::log(L"Island pipe: cannot create the pipe (error %u)", GetLastError());

            if (WaitForSingleObject(g_stop_event, retry_wait_ms) != WAIT_TIMEOUT)
            {
                break;
            }

            continue;
        }

        host::log(L"Island pipe: listening");

        while (WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT)
        {
            OVERLAPPED overlapped = {};
            overlapped.hEvent = event.get();
            const WaitResult connected = connect_client(pipe.get(), overlapped);

            if (connected == WaitResult::Stopped)
            {
                break;
            }

            if (connected == WaitResult::Failed && WaitForSingleObject(g_stop_event, 200) != WAIT_TIMEOUT)
            {
                break;
            }

            if (connected == WaitResult::Completed)
            {
                if (is_trusted_client(pipe.get()))
                {
                    host::log(L"Island pipe: client connected");
                    serve_client(pipe.get(), overlapped);
                    host::log(L"Island pipe: client disconnected");
                }
                else
                {
                    host::log(L"Island pipe: rejected an untrusted client");
                }
            }

            DisconnectNamedPipe(pipe.get());
        }
    }

    return 0;
}

}

}
