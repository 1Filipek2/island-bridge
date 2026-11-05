#include "host/host.h"
#include "island.h"

namespace
{

constexpr wchar_t instance_mutex_name[] = L"Local\\IslandBridge.Island.Instance";
constexpr wchar_t stop_event_name[] = L"Local\\IslandBridge.Island.Stop";
constexpr DWORD settings_settle_ms = 150;

class Handle
{
public:
    explicit Handle(HANDLE value) : value_(value)
    {
    }

    ~Handle()
    {
        if (value_)
        {
            CloseHandle(value_);
        }
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    HANDLE get() const
    {
        return value_;
    }

private:
    HANDLE value_;
};

void wait_for_settings_to_settle(HANDLE changed)
{
    host::rearm_settings_watch();

    while (WaitForSingleObject(changed, settings_settle_ms) == WAIT_OBJECT_0)
    {
        host::rearm_settings_watch();
    }
}

int run()
{
    Handle stop(CreateEventW(nullptr, TRUE, FALSE, stop_event_name));

    if (!stop.get() || !host::start())
    {
        return 1;
    }

    ResetEvent(stop.get());

    if (!island_start())
    {
        island_stop();
        host::stop();
        return 1;
    }

    host::log(L"Island running.");
    const HANDLE changed = host::settings_changed_event();
    int exit_code = 0;

    for (;;)
    {
        const HANDLE waits[] = {stop.get(), changed, island_render_thread()};
        const DWORD count = waits[2] ? 3 : 2;
        const DWORD signaled = WaitForMultipleObjects(count, waits, FALSE, INFINITE);

        if (signaled == WAIT_OBJECT_0 + 1)
        {
            wait_for_settings_to_settle(changed);
            host::refresh_logging();
            island_settings_changed();
            continue;
        }

        if (signaled == WAIT_OBJECT_0 + 2)
        {
            host::log(L"Island render thread ended.");
            exit_code = 2;
        }
        else if (signaled != WAIT_OBJECT_0)
        {
            exit_code = 3;
        }

        break;
    }

    island_stop();
    host::stop();
    return exit_code;
}

}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    Handle instance(CreateMutexW(nullptr, TRUE, instance_mutex_name));

    if (!instance.get() || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        return 0;
    }

    const int exit_code = run();
    ReleaseMutex(instance.get());
    return exit_code;
}
