#include "services/location.h"
#include "services/http.h"
#include "host/host.h"

#include <winrt/Windows.Devices.Geolocation.h>

namespace island::location
{
namespace
{

constexpr wchar_t nominatim_host[] = L"nominatim.openstreetmap.org";
constexpr wchar_t user_agent[] = L"IslandBridge.Island/" ISLAND_VERSION;
constexpr wchar_t privacy_settings[] = L"ms-settings:privacy-location";
constexpr wchar_t service_arguments[] = L"config lfsvc start= demand";
constexpr auto maximum_age = std::chrono::minutes(10);
constexpr auto locate_timeout = std::chrono::seconds(15);
constexpr DWORD fix_timeout_ms = 60000;
constexpr std::array<DWORD, 3> settings_recheck_ms = {15000, 30000, 60000};
constexpr HRESULT service_disabled = HRESULT_FROM_WIN32(ERROR_SERVICE_DISABLED);
constexpr HRESULT access_denied = E_ACCESSDENIED;

std::atomic<Status> current_status{Status::Unknown};
std::atomic<bool> fixing{false};

std::wstring user_language()
{
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};

    if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) <= 0)
    {
        return {};
    }

    const std::wstring name = locale;
    return name.substr(0, name.find(L'-'));
}

void publish(Status value)
{
    if (current_status.exchange(value) != value)
    {
        host::log(L"Location: status %d", static_cast<int>(value));
    }
}

std::wstring service_tool_path()
{
    wchar_t system[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(system, MAX_PATH);

    if (length == 0 || length >= MAX_PATH)
    {
        return {};
    }

    return std::wstring(system, length) + L"\\sc.exe";
}

bool enable_service()
{
    const std::wstring tool = service_tool_path();

    if (tool.empty())
    {
        return false;
    }

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
    info.lpVerb = L"runas";
    info.lpFile = tool.c_str();
    info.lpParameters = service_arguments;
    info.nShow = SW_HIDE;

    if (!ShellExecuteExW(&info) || !info.hProcess)
    {
        return false;
    }

    DWORD exit_code = 1;

    if (WaitForSingleObject(info.hProcess, fix_timeout_ms) == WAIT_OBJECT_0)
    {
        GetExitCodeProcess(info.hProcess, &exit_code);
    }

    CloseHandle(info.hProcess);
    return exit_code == 0;
}

void open_privacy_settings()
{
    ShellExecuteW(nullptr, L"open", privacy_settings, nullptr, nullptr, SW_SHOWNORMAL);
}

}

std::optional<geo::Point> current_point()
{
    using namespace winrt::Windows::Devices::Geolocation;

    try
    {
        const GeolocationAccessStatus access = Geolocator::RequestAccessAsync().get();

        if (access != GeolocationAccessStatus::Allowed)
        {
            publish(Status::Denied);
            return std::nullopt;
        }

        Geolocator locator;
        locator.DesiredAccuracy(PositionAccuracy::Default);
        const Geoposition position = locator.GetGeopositionAsync(maximum_age, locate_timeout).get();
        const BasicGeoposition coordinate = position.Coordinate().Point().Position();
        const geo::Point point{coordinate.Latitude, coordinate.Longitude};

        if (!geo::valid(point))
        {
            publish(Status::Unavailable);
            return std::nullopt;
        }

        publish(Status::Ready);
        return geo::rounded(point);
    }
    catch (const winrt::hresult_error& error)
    {
        const HRESULT code = error.code().value;
        publish(code == service_disabled ? Status::ServiceDisabled
                : code == access_denied  ? Status::Denied
                                         : Status::Unavailable);
        return std::nullopt;
    }
}

std::optional<std::wstring> place_name(geo::Point point)
{
    const std::wstring path = geo::reverse_path(point, user_language());
    const HttpResult result = http_get_ex(nominatim_host, path.c_str(), true, user_agent);

    if (!result.ok || result.status != 200)
    {
        return std::nullopt;
    }

    return geo::parse_place_name(result.body);
}

Status status()
{
    return current_status.load();
}

bool can_fix(Status value)
{
    return value == Status::ServiceDisabled || value == Status::Denied;
}

void fix_access(void* refresh_event)
{
    const Status value = status();

    if (!can_fix(value) || fixing.exchange(true))
    {
        return;
    }

    std::thread(
        [value, refresh_event]
        {
            if (value == Status::Denied)
            {
                open_privacy_settings();

                for (const DWORD delay_ms : settings_recheck_ms)
                {
                    Sleep(delay_ms);

                    if (refresh_event)
                    {
                        SetEvent(static_cast<HANDLE>(refresh_event));
                    }
                }
            }
            else
            {
                const bool enabled = enable_service();
                host::log(L"Location: enabling the service %s", enabled ? L"succeeded" : L"failed");

                if (enabled && refresh_event)
                {
                    SetEvent(static_cast<HANDLE>(refresh_event));
                }
            }

            fixing = false;
        })
        .detach();
}

}
