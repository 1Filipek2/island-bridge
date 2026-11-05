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


DWORD WINAPI NotificationThreadProc(void*);

}

namespace toast_listener
{
namespace
{


std::atomic<bool> enabled = false;

bool should_run()
{
    return enabled.load() && WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT;
}

}
}

namespace toast_listener
{


void sync_thread()
{
    enabled = host::int_setting(L"Modules.WindowsToastListener") != 0;

    if (!enabled.load() || !g_running.load() || !g_stopEvent || WaitForSingleObject(g_stopEvent, 0) != WAIT_TIMEOUT)
    {
        return;
    }

    if (g_notificationThread && WaitForSingleObject(g_notificationThread, 0) == WAIT_OBJECT_0)
    {
        CloseHandle(g_notificationThread);
        g_notificationThread = nullptr;
    }

    if (!g_notificationThread)
    {
        g_notificationThread = CreateThread(nullptr, 0, NotificationThreadProc, nullptr, 0, nullptr);
    }
}

}

namespace
{


DWORD WINAPI NotificationThreadProc(void*) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    using winrt::Windows::UI::Notifications::KnownNotificationBindings;
    using winrt::Windows::UI::Notifications::NotificationKinds;
    using winrt::Windows::UI::Notifications::Management::UserNotificationListener;
    using winrt::Windows::UI::Notifications::Management::UserNotificationListenerAccessStatus;

    std::set<uint32_t> seenIds;
    bool firstPoll = true;
    bool accessLogged = false;

    // The Windows Notification Service (WNS) and UWP subsystem take time to initialize on boot.
    // If this runs too early, instantiating UserNotificationListener::Current()
    // can permanently bind to an uninitialized COM proxy, permanently breaking notifications for the process.
    // To prevent this, we enforce a strict 30-second delay from process creation before touching the API.
    FILETIME creationTime, exitTime, kernelTime, userTime;
    if (GetProcessTimes(GetCurrentProcess(), &creationTime, &exitTime, &kernelTime, &userTime)) {
        ULARGE_INTEGER ct;
        ct.LowPart = creationTime.dwLowDateTime;
        ct.HighPart = creationTime.dwHighDateTime;

        FILETIME systemTime;
        GetSystemTimeAsFileTime(&systemTime);
        ULARGE_INTEGER st;
        st.LowPart = systemTime.dwLowDateTime;
        st.HighPart = systemTime.dwHighDateTime;

        uint64_t msSinceProcessStart = (st.QuadPart - ct.QuadPart) / 10000;
        if (msSinceProcessStart < 30000) {
            DWORD waitTime = 30000 - (DWORD)msSinceProcessStart;
            host::log(L"Process started recently. Delaying UserNotificationListener init by %d ms to let UWP subsystem load...", waitTime);
            WaitForSingleObject(g_stopEvent, waitTime);
        }
    }

    while (toast_listener::should_run()) {
        try {
            auto listener = UserNotificationListener::Current();
            auto access = listener.RequestAccessAsync().get();
            if (access != UserNotificationListenerAccessStatus::Allowed) {
                if (!accessLogged) {
                    if (access == UserNotificationListenerAccessStatus::Denied) {
                        // Actionable: this is a Windows privacy switch, not a
                        // mod setting, and it is the usual reason the module
                        // stays silent when everything else is configured.
                        host::log(L"Notification listener permission DENIED by Windows. "
                               L"Enable Settings > Privacy & security > Notifications > "
                               L"\"Let apps access your notifications\", then restart the mod. "
                               L"Retrying meanwhile...");
                    } else {
                        host::log(L"Notification listener permission not granted or UWP subsystem not ready on boot (status: %d); retrying connection loop...", (int)access);
                    }
                    accessLogged = true;
                }
                WaitForSingleObject(g_stopEvent, 3000);
                continue;
            }

            if (accessLogged) {
                host::log(L"WinRT UserNotificationListener successfully connected.");
                accessLogged = false;
            }

            while (toast_listener::should_run()) {
                try {
                    auto notifications = listener.GetNotificationsAsync(NotificationKinds::Toast).get();
                    std::set<uint32_t> currentIds;

                    for (uint32_t i = 0; i < notifications.Size(); ++i) {
                        currentIds.insert(notifications.GetAt(i).Id());
                    }

                    if (firstPoll) {
                        host::log(L"Notifications: listener ready, %u existing toasts ignored, DnD=%d",
                               notifications.Size(), (int)g_isDnDActive.load());
                        seenIds = std::move(currentIds);
                        firstPoll = false;
                        WaitForSingleObject(g_stopEvent, 1000);
                        continue;
                    }

                    for (uint32_t i = 0; i < notifications.Size(); ++i) {
                        try {
                            auto userNotification = notifications.GetAt(i);
                            const uint32_t id = userNotification.Id();

                            if (seenIds.count(id)) {
                                continue;
                            }

                            // Immediately mark as seen so we don't process it again
                            seenIds.insert(id);

                            if (g_settings.notificationRespectDnD && g_isDnDActive.load()) {
                                continue;
                            }

                            NotificationSnapshot snapshot;
                            snapshot.active = true;
                            snapshot.expiresAt = NowSeconds() + 4.0;
                            winrt::Windows::ApplicationModel::AppInfo appInfo{nullptr};
                            winrt::Windows::ApplicationModel::AppDisplayInfo displayInfo{nullptr};
                            try {
                                appInfo = userNotification.AppInfo();
                                displayInfo = appInfo.DisplayInfo();
                                snapshot.app = displayInfo.DisplayName().c_str();
                            } catch (const winrt::hresult_error& appInfoError) {
                                host::log(L"Notifications: app info failed (0x%08X)", static_cast<unsigned>(appInfoError.to_abi()));
                            } catch (...) {
                                host::log(L"Notifications: app info failed (unknown)");
                            }

                            // Filter out noisy background console/script engines like PowerShell that fire system toasts unexpectedly
                            const std::wstring lowerApp = ToLowerCopy(snapshot.app);
                            if (lowerApp.find(L"powershell") != std::wstring::npos ||
                                lowerApp.find(L"pwsh") != std::wstring::npos ||
                                lowerApp.find(L"cmd.exe") != std::wstring::npos) {
                                continue;
                            }
                            if (displayInfo) try {
                                auto logo = displayInfo.GetLogo({32.0f, 32.0f});
                                std::vector<uint8_t> logoBytes = ReadWinRtStreamBytes(logo);
                                if (!logoBytes.empty()) {
                                    BitmapPixels decoded;
                                    if (DecodeImageBytesToPixels(logoBytes, &decoded)) {
                                        snapshot.icon = PixelsRef(std::move(decoded));
                                    }
                                }
                            } catch (...) {
                            }

                            if (!snapshot.icon && !snapshot.app.empty()) {
                                snapshot.icon = FindAppIconByName(snapshot.app, 64);
                            }

                            auto notification = userNotification.Notification();
                            auto binding = notification.Visual().GetBinding(KnownNotificationBindings::ToastGeneric());
                            if (binding) {
                                auto textElements = binding.GetTextElements();
                                std::vector<std::wstring> texts;
                                for (uint32_t t = 0; t < textElements.Size(); ++t) {
                                    std::wstring text = textElements.GetAt(t).Text().c_str();
                                    const size_t b = text.find_first_not_of(L" \t\r\n");
                                    text = b == std::wstring::npos
                                               ? std::wstring()
                                               : text.substr(b, text.find_last_not_of(L" \t\r\n") - b + 1);
                                    for (wchar_t& ch : text) {
                                        if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
                                    }
                                    texts.push_back(std::move(text));
                                }

                                // Fork: Phone Link (iPhone) toasts are laid out as
                                // [phone app, sender/subject, message...]. Show the
                                // phone's app as the source instead of "Phone Link".
                                bool isPhoneLink = lowerApp.find(L"phone link") != std::wstring::npos;
                                try {
                                    isPhoneLink = isPhoneLink ||
                                        (appInfo && ToLowerCopy(appInfo.PackageFamilyName().c_str()).find(L"microsoft.yourphone") !=
                                            std::wstring::npos);
                                } catch (...) {
                                }
                                if (isPhoneLink && texts.size() >= 2) {
                                    snapshot.app = texts[0].empty() ? L"iPhone" : texts[0];
                                    texts.erase(texts.begin());
                                }

                                for (const std::wstring& text : texts) {
                                    if (text.empty()) continue;
                                    if (snapshot.title.empty()) {
                                        snapshot.title = text;
                                    } else {
                                        snapshot.body += (snapshot.body.empty() ? L"" : L" ") + text;
                                    }
                                }
                            }

                            if (snapshot.title.empty()) {
                                snapshot.title = snapshot.app.empty() ? L"New notification" : snapshot.app;
                            }
                            if (!snapshot.body.empty()) {
                                snapshot.title += L" - " + snapshot.body;
                            }
                            if (snapshot.title.size() > 120) {
                                snapshot.title.resize(120);
                                snapshot.title += L"...";
                            }

                            // Start the 4 s display window only now; icon lookup above can take a while.
                            snapshot.expiresAt = NowSeconds() + 4.0;
                            {
                                std::lock_guard lock(g_stateMutex);
                                g_state.notification = std::move(snapshot);
                            }
                            TriggerNudge();
                        } catch (const winrt::hresult_error& nex) {
                            host::log(L"Failed to parse a notification (0x%08X); skipping.", static_cast<unsigned>(nex.to_abi()));
                        } catch (...) {
                            host::log(L"Failed to parse a notification; skipping.");
                        }
                    }

                    seenIds = std::move(currentIds);
                } catch (const winrt::hresult_error& ex) {
                    const HRESULT hr = ex.to_abi();
                    if (hr == static_cast<HRESULT>(0x80004001) || hr == static_cast<HRESULT>(0x80040154)) { // E_NOTIMPL or REGDB_E_CLASSNOTREG
                        if (!accessLogged) {
                            host::log(L"Notification listener UWP subsystem not fully ready (0x%08X). Retrying in background...", hr);
                            accessLogged = true;
                        }
                    } else {
                        host::log(L"NotificationThreadProc inner loop WinRT error: %s (0x%08X); reconnecting...", ex.message().c_str(), hr);
                    }
                    WaitForSingleObject(g_stopEvent, 3000);
                    break;
                } catch (...) {
                    host::log(L"NotificationThreadProc inner loop unknown exception; reconnecting...");
                    WaitForSingleObject(g_stopEvent, 3000);
                    break;
                }

                WaitForSingleObject(g_stopEvent, 1000);
            }
        } catch (const winrt::hresult_error& ex) {
            if (!accessLogged) {
                host::log(L"NotificationThreadProc connection error: %s (0x%08X). Retrying in 3s...", ex.message().c_str(), ex.to_abi());
                accessLogged = true;
            }
            WaitForSingleObject(g_stopEvent, 3000);
        } catch (...) {
            if (!accessLogged) {
                host::log(L"NotificationThreadProc unknown connection exception. Retrying in 3s...");
                accessLogged = true;
            }
            WaitForSingleObject(g_stopEvent, 3000);
        }
    }

    winrt::uninit_apartment();
    return 0;
}

}

}
