#include "notifications/dnd.h"
#include "core/model.h"
#include "core/state.h"
#include "core/util.h"

namespace island
{
namespace
{

typedef LONG NTSTATUS;
typedef NTSTATUS(NTAPI* PWNF_USER_CALLBACK)(ULONG64 state_name, ULONG change_stamp, void* type_id,
                                            void* callback_context, const void* buffer, ULONG buffer_size);

typedef NTSTATUS(NTAPI* PFN_RtlSubscribeWnfStateChangeNotification)(void** subscription, ULONG64 state_name,
                                                                    ULONG change_stamp, PWNF_USER_CALLBACK callback,
                                                                    void* callback_context, const void* type_id,
                                                                    ULONG serialization_group, ULONG unknown);

typedef NTSTATUS(NTAPI* PFN_RtlUnsubscribeWnfStateChangeNotification)(void* subscription);

typedef NTSTATUS(NTAPI* PFN_NtQueryWnfStateData)(const ULONG64* state_name, const void* type_id,
                                                 const void* explicit_scope, ULONG* change_stamp, void* buffer,
                                                 ULONG* buffer_size);

constexpr ULONG64 wnf_quiet_hours_active_profile_changed = 0xD83063EA3BF1C75ULL;

NTSTATUS NTAPI wnf_dnd_callback(ULONG64 state_name, ULONG change_stamp, void* type_id, void* callback_context,
                                const void* buffer, ULONG buffer_size)
{
    if (state_name != wnf_quiet_hours_active_profile_changed)
    {
        return 0;
    }
    int val = 0;
    if (buffer && buffer_size >= sizeof(int))
    {
        val = *reinterpret_cast<const int*>(buffer);
    }
    const bool active = (val != 0);
    static std::atomic<bool> s_first_wnf = true;
    if (s_first_wnf.exchange(false))
    {
        g_is_dn_d_active.store(active);
        return 0;
    }

    const bool prev = g_is_dn_d_active.exchange(active);
    if (prev != active && g_settings.do_not_disturb_indicator)
    {
        {
            std::lock_guard lock(g_state_mutex);
            g_state.do_not_disturb.active = true;
            g_state.do_not_disturb.enabled = active;
            g_state.do_not_disturb.expires_at = now_seconds() + 3.0;
        }
        trigger_nudge();
    }
    return 0;
}

}

void subscribe_dnd_notification()
{
    HMODULE h_ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!h_ntdll)
    {
        return;
    }

    auto pfn_query = reinterpret_cast<PFN_NtQueryWnfStateData>(GetProcAddress(h_ntdll, "NtQueryWnfStateData"));
    if (pfn_query)
    {
        ULONG stamp = 0;
        ULONG size = sizeof(int);
        int val = 0;
        ULONG64 state_name = wnf_quiet_hours_active_profile_changed;
        if (pfn_query(&state_name, nullptr, nullptr, &stamp, &val, &size) == 0 && size >= sizeof(int))
        {
            g_is_dn_d_active.store(val != 0);
        }
    }

    auto pfn_subscribe = reinterpret_cast<PFN_RtlSubscribeWnfStateChangeNotification>(
        GetProcAddress(h_ntdll, "RtlSubscribeWnfStateChangeNotification"));
    if (!pfn_subscribe)
    {
        return;
    }

    pfn_subscribe(&g_wnf_dnd_subscription, wnf_quiet_hours_active_profile_changed, 0, wnf_dnd_callback, nullptr,
                  nullptr, 0, 0);
}

void unsubscribe_dnd_notification()
{
    if (!g_wnf_dnd_subscription)
    {
        return;
    }
    HMODULE h_ntdll = GetModuleHandleW(L"ntdll.dll");
    if (h_ntdll)
    {
        auto pfn_unsubscribe = reinterpret_cast<PFN_RtlUnsubscribeWnfStateChangeNotification>(
            GetProcAddress(h_ntdll, "RtlUnsubscribeWnfStateChangeNotification"));
        if (pfn_unsubscribe)
        {
            pfn_unsubscribe(g_wnf_dnd_subscription);
        }
    }
    g_wnf_dnd_subscription = nullptr;
}

}
