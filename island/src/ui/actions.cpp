#include "ui/actions.h"
#include "ui/icons.h"
#include "core/state.h"
#include "core/util.h"

namespace island
{
void set_click_through(HWND hwnd, bool click_through)
{
    LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const bool has = (ex_style & WS_EX_TRANSPARENT) != 0;
    if (click_through == has)
    {
        return;
    }

    if (click_through)
    {
        ex_style |= WS_EX_TRANSPARENT;
    }
    else
    {
        ex_style &= ~WS_EX_TRANSPARENT;
    }

    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex_style);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void handle_status_click_at_point(HWND hwnd, LPARAM l_param)
{
    return;
}

void start_focus_timer(int minutes, bool is_break)
{
    {
        std::lock_guard lock(g_state_mutex);
        g_state.timer.active = true;
        g_state.timer.running = true;
        g_state.timer.is_break = is_break;
        g_state.timer.total_seconds = minutes * 60;
        g_state.timer.ends_at = now_seconds() + minutes * 60;
        g_state.timer.remaining_at_pause = 0.0;
        g_state.timer.just_finished = false;
    }
    trigger_nudge();
}

void toggle_timer_pause()
{
    {
        std::lock_guard lock(g_state_mutex);
        if (!g_state.timer.active)
        {
            return;
        }
        const double now = now_seconds();
        if (g_state.timer.running)
        {
            g_state.timer.remaining_at_pause = std::max(0.0, g_state.timer.ends_at - now);
            g_state.timer.running = false;
        }
        else
        {
            g_state.timer.ends_at = now + g_state.timer.remaining_at_pause;
            g_state.timer.running = true;
        }
    }
    trigger_nudge();
}

void stop_focus_timer()
{
    {
        std::lock_guard lock(g_state_mutex);
        g_state.timer.active = false;
        g_state.timer.running = false;
        g_state.timer.just_finished = false;
    }
    trigger_nudge();
}

void toggle_endpoint_mute()
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioEndpointVolume> volume;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr))
    {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    }
    if (SUCCEEDED(hr))
    {
        hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(volume.GetAddressOf()));
    }
    if (SUCCEEDED(hr))
    {
        BOOL muted = FALSE;
        volume->GetMute(&muted);
        volume->SetMute(!muted, nullptr);
        std::lock_guard lock(g_state_mutex);
        g_state.muted = !muted;
    }
}

void seek_media_to_ticks(std::int64_t target_ticks)
{
    std::thread(
        [target_ticks]()
        {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            try
            {
                using Manager = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
                auto manager = Manager::RequestAsync().get();
                if (manager)
                {
                    auto sessions = manager.GetSessions();
                    std::wstring current_aumid;
                    {
                        std::lock_guard lock(g_state_mutex);
                        current_aumid = g_state.media.source_app_user_model_id;
                    }
                    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession session = nullptr;
                    for (auto const& s : sessions)
                    {
                        if (s.SourceAppUserModelId().c_str() == current_aumid)
                        {
                            session = s;
                            break;
                        }
                    }
                    if (!session)
                    {
                        session = manager.GetCurrentSession();
                    }

                    if (session)
                    {
                        session.TryChangePlaybackPositionAsync(target_ticks).get();
                    }
                }
            }
            catch (...)
            {
            }
        })
        .detach();
}

namespace
{
winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession resolve_media_session()
{
    using Manager = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
    auto manager = Manager::RequestAsync().get();
    if (!manager)
    {
        return nullptr;
    }

    std::wstring current_aumid;
    {
        std::lock_guard lock(g_state_mutex);
        current_aumid = g_state.media.source_app_user_model_id;
    }

    if (!current_aumid.empty())
    {
        for (auto const& s : manager.GetSessions())
        {
            if (s.SourceAppUserModelId().c_str() == current_aumid)
            {
                return s;
            }
        }
    }
    return manager.GetCurrentSession();
}

}

// Transport command: 0 = previous, 1 = play/pause, 2 = next. Only SMTC when a session exists; media
// keys are sent only when SMTC could not act at all, because the shell routes them to its own
// "current" session and a player could be driven twice.
void send_media_transport_command(int cmd)
{
    std::thread(
        [cmd]()
        {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);

            bool session_attempted = false;
            bool reported_success = false;
            try
            {
                auto session = resolve_media_session();
                if (session)
                {
                    session_attempted = true;
                    if (cmd == 0)
                    {
                        reported_success = session.TrySkipPreviousAsync().get();
                    }
                    else if (cmd == 1)
                    {
                        reported_success = session.TryTogglePlayPauseAsync().get();
                    }
                    else if (cmd == 2)
                    {
                        reported_success = session.TrySkipNextAsync().get();
                    }
                }
            }
            catch (...)
            {
                session_attempted = false;
            }

            if (session_attempted)
            {
                if (!reported_success)
                {
                    host::log(L"Media transport command %d was refused by the session.", cmd);
                }
                return;
            }

            BYTE vk = 0;
            if (cmd == 0)
            {
                vk = VK_MEDIA_PREV_TRACK;
            }
            else if (cmd == 1)
            {
                vk = VK_MEDIA_PLAY_PAUSE;
            }
            else if (cmd == 2)
            {
                vk = VK_MEDIA_NEXT_TRACK;
            }
            if (vk != 0)
            {
                keybd_event(vk, 0, KEYEVENTF_EXTENDEDKEY, 0);
                keybd_event(vk, 0, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0);
            }
        })
        .detach();
}

namespace
{
struct WindowSearch
{
    std::wstring target_title;
    std::wstring target_app;
    HWND found_hwnd = nullptr;
    HWND fallback_hwnd = nullptr;
};

BOOL CALLBACK enum_windows_proc(HWND hwnd, LPARAM l_param)
{
    if (!IsWindowVisible(hwnd))
    {
        return TRUE;
    }

    auto* search = reinterpret_cast<WindowSearch*>(l_param);

    wchar_t title[512];
    if (GetWindowTextW(hwnd, title, ARRAYSIZE(title)) > 0)
    {
        std::wstring w_title(title);
        auto it = std::search(w_title.begin(), w_title.end(), search->target_title.begin(), search->target_title.end(),
                              [](wchar_t ch1, wchar_t ch2)
                              {
                                  return std::towlower(ch1) == std::towlower(ch2);
                              });

        if (it != w_title.end())
        {
            search->found_hwnd = hwnd;
            return FALSE; // found exact title, stop enumerating
        }
    }

    if (!search->fallback_hwnd && !search->target_app.empty())
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != 0)
        {
            std::wstring exe_path;
            if (process_image_name_for_pid(pid, &exe_path))
            {
                std::wstring target_lower = to_lower_copy(search->target_app);
                std::wstring exe_lower = to_lower_copy(exe_path);

                if (target_lower.size() >= 2 && target_lower.front() == L'"' && target_lower.back() == L'"')
                {
                    target_lower = target_lower.substr(1, target_lower.size() - 2);
                }

                if (exe_lower == target_lower)
                {
                    search->fallback_hwnd = hwnd;
                }
                else
                {
                    std::wstring exe_name = exe_lower;
                    std::size_t slash_pos = exe_name.find_last_of(L"\\/");
                    if (slash_pos != std::wstring::npos)
                    {
                        exe_name = exe_name.substr(slash_pos + 1);
                    }
                    if (exe_name == target_lower || exe_name == target_lower + L".exe")
                    {
                        search->fallback_hwnd = hwnd;
                    }
                }
            }
        }
    }

    return TRUE;
}

}

void open_relevant_app()
{
    std::wstring title;
    std::wstring app;
    {
        std::lock_guard lock(g_state_mutex);
        title = g_state.media.title;
        app = g_state.media.source_app_user_model_id;
    }

    if (!title.empty() || !app.empty())
    {
        WindowSearch search;
        search.target_title = title;
        search.target_app = app;
        EnumWindows(enum_windows_proc, reinterpret_cast<LPARAM>(&search));

        HWND hwnd_to_focus = search.found_hwnd ? search.found_hwnd : search.fallback_hwnd;

        if (hwnd_to_focus)
        {
            if (IsIconic(hwnd_to_focus))
            {
                ShowWindow(hwnd_to_focus, SW_RESTORE);
            }
            SetForegroundWindow(hwnd_to_focus);
            return;
        }
    }

    if (!app.empty())
    {
        std::wstring execute_path = app;

        if (execute_path.size() >= 2 && execute_path.front() == L'"' && execute_path.back() == L'"')
        {
            execute_path = execute_path.substr(1, execute_path.size() - 2);
        }

        bool is_file_path = (execute_path.find(L":\\") != std::wstring::npos ||
                             (execute_path.size() >= 4 && execute_path.substr(execute_path.size() - 4) == L".exe"));

        if (is_file_path)
        {
            if (GetFileAttributesW(execute_path.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                std::size_t x86_pos = execute_path.find(L" (x86)");
                if (x86_pos != std::wstring::npos)
                {
                    std::wstring alt_path = execute_path;
                    alt_path.erase(x86_pos, 6);
                    if (GetFileAttributesW(alt_path.c_str()) != INVALID_FILE_ATTRIBUTES)
                    {
                        execute_path = alt_path;
                    }
                }

                // Gives up when the path does not exist: guessing an exe name triggers broken App Paths entries.
                if (GetFileAttributesW(execute_path.c_str()) == INVALID_FILE_ATTRIBUTES)
                {
                    return;
                }
            }

            SHELLEXECUTEINFOW sei = {sizeof(sei)};
            sei.fMask = SEE_MASK_FLAG_NO_UI;
            sei.lpFile = execute_path.c_str();
            sei.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&sei);
        }
        else
        {
            std::wstring shell_path = L"shell:AppsFolder\\" + execute_path;
            SHELLEXECUTEINFOW sei = {sizeof(sei)};
            sei.fMask = SEE_MASK_FLAG_NO_UI;
            sei.lpFile = shell_path.c_str();
            sei.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&sei);
        }
        return;
    }

    ShellExecuteW(nullptr, L"open", L"ms-settings:", nullptr, nullptr, SW_SHOWNORMAL);
}

void dismiss_transient_state()
{
    std::lock_guard lock(g_state_mutex);
    g_state.clipboard.active = false;
    g_state.notification.active = false;
    g_state.volume.active = false;
    g_state.brightness.active = false;
    g_state.progress.active = false;
    g_state.caps_lock.active = false;
    g_state.device.active = false;
    g_state.bluetooth_device.active = false;
    g_state.battery.active = false;
    host::set_int_value(L"ProgressPercent", -1);
}

}
