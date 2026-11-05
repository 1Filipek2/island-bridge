#include "notifications/toast_listener.h"
#include "ui/icons.h"
#include "ui/image.h"
#include "core/model.h"
#include "core/pixels.h"
#include "core/state.h"
#include "core/util.h"

namespace island
{
namespace
{

DWORD WINAPI notification_thread_proc(void*);

}

namespace toast_listener
{
namespace
{

std::atomic<bool> enabled = false;

bool should_run()
{
    return enabled.load() && WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT;
}

}
}

namespace toast_listener
{

void sync_thread()
{
    enabled = host::int_setting(L"Modules.WindowsToastListener") != 0;

    if (!enabled.load() || !g_running.load() || !g_stop_event || WaitForSingleObject(g_stop_event, 0) != WAIT_TIMEOUT)
    {
        return;
    }

    if (g_notification_thread && WaitForSingleObject(g_notification_thread, 0) == WAIT_OBJECT_0)
    {
        CloseHandle(g_notification_thread);
        g_notification_thread = nullptr;
    }

    if (!g_notification_thread)
    {
        g_notification_thread = CreateThread(nullptr, 0, notification_thread_proc, nullptr, 0, nullptr);
    }
}

}

namespace
{

DWORD WINAPI notification_thread_proc(void*)
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    using winrt::Windows::UI::Notifications::KnownNotificationBindings;
    using winrt::Windows::UI::Notifications::NotificationKinds;
    using winrt::Windows::UI::Notifications::Management::UserNotificationListener;
    using winrt::Windows::UI::Notifications::Management::UserNotificationListenerAccessStatus;

    std::set<uint32_t> seen_ids;
    bool first_poll = true;
    bool access_logged = false;

    // The Windows Notification Service (WNS) and UWP subsystem take time to initialize on boot.
    // If this runs too early, instantiating UserNotificationListener::Current()
    // can permanently bind to an uninitialized COM proxy, permanently breaking notifications for the process.
    // To prevent this, we enforce a strict 30-second delay from process creation before touching the API.
    FILETIME creation_time, exit_time, kernel_time, user_time;
    if (GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time, &kernel_time, &user_time))
    {
        ULARGE_INTEGER ct;
        ct.LowPart = creation_time.dwLowDateTime;
        ct.HighPart = creation_time.dwHighDateTime;

        FILETIME system_time;
        GetSystemTimeAsFileTime(&system_time);
        ULARGE_INTEGER st;
        st.LowPart = system_time.dwLowDateTime;
        st.HighPart = system_time.dwHighDateTime;

        uint64_t ms_since_process_start = (st.QuadPart - ct.QuadPart) / 10000;
        if (ms_since_process_start < 30000)
        {
            DWORD wait_time = 30000 - (DWORD)ms_since_process_start;
            host::log(L"Process started recently. Delaying UserNotificationListener init by %d ms to let UWP subsystem "
                      L"load...",
                      wait_time);
            WaitForSingleObject(g_stop_event, wait_time);
        }
    }

    while (toast_listener::should_run())
    {
        try
        {
            auto listener = UserNotificationListener::Current();
            auto access = listener.RequestAccessAsync().get();
            if (access != UserNotificationListenerAccessStatus::Allowed)
            {
                if (!access_logged)
                {
                    if (access == UserNotificationListenerAccessStatus::Denied)
                    {
                        // Actionable: this is a Windows privacy switch, not a
                        // mod setting, and it is the usual reason the module
                        // stays silent when everything else is configured.
                        host::log(L"Notification listener permission DENIED by Windows. "
                                  L"Enable Settings > Privacy & security > Notifications > "
                                  L"\"Let apps access your notifications\", then restart the mod. "
                                  L"Retrying meanwhile...");
                    }
                    else
                    {
                        host::log(L"Notification listener permission not granted or UWP subsystem not ready on boot "
                                  L"(status: %d); retrying connection loop...",
                                  (int)access);
                    }
                    access_logged = true;
                }
                WaitForSingleObject(g_stop_event, 3000);
                continue;
            }

            if (access_logged)
            {
                host::log(L"WinRT UserNotificationListener successfully connected.");
                access_logged = false;
            }

            while (toast_listener::should_run())
            {
                try
                {
                    auto notifications = listener.GetNotificationsAsync(NotificationKinds::Toast).get();
                    std::set<uint32_t> current_ids;

                    for (uint32_t i = 0; i < notifications.Size(); ++i)
                    {
                        current_ids.insert(notifications.GetAt(i).Id());
                    }

                    if (first_poll)
                    {
                        host::log(L"Notifications: listener ready, %u existing toasts ignored, DnD=%d",
                                  notifications.Size(), (int)g_is_dn_d_active.load());
                        seen_ids = std::move(current_ids);
                        first_poll = false;
                        WaitForSingleObject(g_stop_event, 1000);
                        continue;
                    }

                    for (uint32_t i = 0; i < notifications.Size(); ++i)
                    {
                        try
                        {
                            auto user_notification = notifications.GetAt(i);
                            const uint32_t id = user_notification.Id();

                            if (seen_ids.count(id))
                            {
                                continue;
                            }

                            // Immediately mark as seen so we don't process it again
                            seen_ids.insert(id);

                            if (g_settings.notification_respect_dn_d && g_is_dn_d_active.load())
                            {
                                continue;
                            }

                            NotificationSnapshot snapshot;
                            snapshot.active = true;
                            snapshot.expires_at = now_seconds() + 4.0;
                            winrt::Windows::ApplicationModel::AppInfo app_info{nullptr};
                            winrt::Windows::ApplicationModel::AppDisplayInfo display_info{nullptr};
                            try
                            {
                                app_info = user_notification.AppInfo();
                                display_info = app_info.DisplayInfo();
                                snapshot.app = display_info.DisplayName().c_str();
                            }
                            catch (const winrt::hresult_error& app_info_error)
                            {
                                host::log(L"Notifications: app info failed (0x%08X)",
                                          static_cast<unsigned>(app_info_error.to_abi()));
                            }
                            catch (...)
                            {
                                host::log(L"Notifications: app info failed (unknown)");
                            }

                            // Filter out noisy background console/script engines like PowerShell that fire system toasts unexpectedly
                            const std::wstring lower_app = to_lower_copy(snapshot.app);
                            if (lower_app.find(L"powershell") != std::wstring::npos ||
                                lower_app.find(L"pwsh") != std::wstring::npos ||
                                lower_app.find(L"cmd.exe") != std::wstring::npos)
                            {
                                continue;
                            }
                            if (display_info)
                            {
                                try
                                {
                                    auto logo = display_info.GetLogo({32.0f, 32.0f});
                                    std::vector<uint8_t> logo_bytes = read_win_rt_stream_bytes(logo);
                                    if (!logo_bytes.empty())
                                    {
                                        BitmapPixels decoded;
                                        if (decode_image_bytes_to_pixels(logo_bytes, &decoded))
                                        {
                                            snapshot.icon = PixelsRef(std::move(decoded));
                                        }
                                    }
                                }
                                catch (...)
                                {
                                }
                            }

                            if (!snapshot.icon && !snapshot.app.empty())
                            {
                                snapshot.icon = find_app_icon_by_name(snapshot.app, 64);
                            }

                            auto notification = user_notification.Notification();
                            auto binding = notification.Visual().GetBinding(KnownNotificationBindings::ToastGeneric());
                            if (binding)
                            {
                                auto text_elements = binding.GetTextElements();
                                std::vector<std::wstring> texts;
                                for (uint32_t t = 0; t < text_elements.Size(); ++t)
                                {
                                    std::wstring text = text_elements.GetAt(t).Text().c_str();
                                    const size_t b = text.find_first_not_of(L" \t\r\n");
                                    text = b == std::wstring::npos
                                               ? std::wstring()
                                               : text.substr(b, text.find_last_not_of(L" \t\r\n") - b + 1);
                                    for (wchar_t& ch : text)
                                    {
                                        if (ch == L'\r' || ch == L'\n' || ch == L'\t')
                                        {
                                            ch = L' ';
                                        }
                                    }
                                    texts.push_back(std::move(text));
                                }

                                // Fork: Phone Link (iPhone) toasts are laid out as
                                // [phone app, sender/subject, message...]. Show the
                                // phone's app as the source instead of "Phone Link".
                                bool is_phone_link = lower_app.find(L"phone link") != std::wstring::npos;
                                try
                                {
                                    is_phone_link =
                                        is_phone_link ||
                                        (app_info && to_lower_copy(app_info.PackageFamilyName().c_str())
                                                             .find(L"microsoft.yourphone") != std::wstring::npos);
                                }
                                catch (...)
                                {
                                }
                                if (is_phone_link && texts.size() >= 2)
                                {
                                    snapshot.app = texts[0].empty() ? L"iPhone" : texts[0];
                                    texts.erase(texts.begin());
                                }

                                for (const std::wstring& text : texts)
                                {
                                    if (text.empty())
                                    {
                                        continue;
                                    }
                                    if (snapshot.title.empty())
                                    {
                                        snapshot.title = text;
                                    }
                                    else
                                    {
                                        snapshot.body += (snapshot.body.empty() ? L"" : L" ") + text;
                                    }
                                }
                            }

                            if (snapshot.title.empty())
                            {
                                snapshot.title = snapshot.app.empty() ? L"New notification" : snapshot.app;
                            }
                            if (!snapshot.body.empty())
                            {
                                snapshot.title += L" - " + snapshot.body;
                            }
                            if (snapshot.title.size() > 120)
                            {
                                snapshot.title.resize(120);
                                snapshot.title += L"...";
                            }

                            // Start the 4 s display window only now; icon lookup above can take a while.
                            snapshot.expires_at = now_seconds() + 4.0;
                            {
                                std::lock_guard lock(g_state_mutex);
                                g_state.notification = std::move(snapshot);
                            }
                            trigger_nudge();
                        }
                        catch (const winrt::hresult_error& nex)
                        {
                            host::log(L"Failed to parse a notification (0x%08X); skipping.",
                                      static_cast<unsigned>(nex.to_abi()));
                        }
                        catch (...)
                        {
                            host::log(L"Failed to parse a notification; skipping.");
                        }
                    }

                    seen_ids = std::move(current_ids);
                }
                catch (const winrt::hresult_error& ex)
                {
                    const HRESULT hr = ex.to_abi();
                    if (hr == static_cast<HRESULT>(0x80004001) || hr == static_cast<HRESULT>(0x80040154))
                    { // E_NOTIMPL or REGDB_E_CLASSNOTREG
                        if (!access_logged)
                        {
                            host::log(L"Notification listener UWP subsystem not fully ready (0x%08X). Retrying in "
                                      L"background...",
                                      hr);
                            access_logged = true;
                        }
                    }
                    else
                    {
                        host::log(L"NotificationThreadProc inner loop WinRT error: %s (0x%08X); reconnecting...",
                                  ex.message().c_str(), hr);
                    }
                    WaitForSingleObject(g_stop_event, 3000);
                    break;
                }
                catch (...)
                {
                    host::log(L"NotificationThreadProc inner loop unknown exception; reconnecting...");
                    WaitForSingleObject(g_stop_event, 3000);
                    break;
                }

                WaitForSingleObject(g_stop_event, 1000);
            }
        }
        catch (const winrt::hresult_error& ex)
        {
            if (!access_logged)
            {
                host::log(L"NotificationThreadProc connection error: %s (0x%08X). Retrying in 3s...",
                          ex.message().c_str(), ex.to_abi());
                access_logged = true;
            }
            WaitForSingleObject(g_stop_event, 3000);
        }
        catch (...)
        {
            if (!access_logged)
            {
                host::log(L"NotificationThreadProc unknown connection exception. Retrying in 3s...");
                access_logged = true;
            }
            WaitForSingleObject(g_stop_event, 3000);
        }
    }

    winrt::uninit_apartment();
    return 0;
}

}

}
