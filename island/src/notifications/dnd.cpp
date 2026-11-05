#include "notifications/dnd.h"
#include "core/model.h"
#include "core/state.h"
#include "core/util.h"

namespace island
{
namespace
{


typedef LONG NTSTATUS;
typedef NTSTATUS (NTAPI *PWNF_USER_CALLBACK)(
    ULONG64 StateName,
    ULONG ChangeStamp,
    void* TypeId,
    void* CallbackContext,
    const void* Buffer,
    ULONG BufferSize
);

typedef NTSTATUS(NTAPI* PFN_RtlSubscribeWnfStateChangeNotification)(
    void** Subscription,
    ULONG64 StateName,
    ULONG ChangeStamp,
    PWNF_USER_CALLBACK Callback,
    void* CallbackContext,
    const void* TypeId,
    ULONG SerializationGroup,
    ULONG Unknown
);

typedef NTSTATUS(NTAPI* PFN_RtlUnsubscribeWnfStateChangeNotification)(
    void* Subscription
);

typedef NTSTATUS(NTAPI* PFN_NtQueryWnfStateData)(
    const ULONG64* StateName,
    const void* TypeId,
    const void* ExplicitScope,
    ULONG* ChangeStamp,
    void* Buffer,
    ULONG* BufferSize
);

constexpr ULONG64 kWnfQuietHoursActiveProfileChanged = 0xD83063EA3BF1C75ULL;

NTSTATUS NTAPI WnfDndCallback(
    ULONG64 stateName,
    ULONG changeStamp,
    void* typeId,
    void* callbackContext,
    const void* buffer,
    ULONG bufferSize
) {
    if (stateName != kWnfQuietHoursActiveProfileChanged) return 0;
    int val = 0;
    if (buffer && bufferSize >= sizeof(int)) {
        val = *reinterpret_cast<const int*>(buffer);
    }
    const bool active = (val != 0);
    static std::atomic<bool> s_firstWnf = true;
    if (s_firstWnf.exchange(false)) {
        g_isDnDActive.store(active);
        return 0;
    }

    const bool prev = g_isDnDActive.exchange(active);
    if (prev != active && g_settings.doNotDisturbIndicator) {
        {
            std::lock_guard lock(g_stateMutex);
            g_state.doNotDisturb.active = true;
            g_state.doNotDisturb.enabled = active;
            g_state.doNotDisturb.expiresAt = NowSeconds() + 3.0;
        }
        TriggerNudge();
    }
    return 0;
}

}



void SubscribeDndNotification() {
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtdll) return;

    auto pfnQuery = reinterpret_cast<PFN_NtQueryWnfStateData>(
        GetProcAddress(hNtdll, "NtQueryWnfStateData"));
    if (pfnQuery) {
        ULONG stamp = 0;
        ULONG size = sizeof(int);
        int val = 0;
        ULONG64 stateName = kWnfQuietHoursActiveProfileChanged;
        if (pfnQuery(&stateName, nullptr, nullptr, &stamp, &val, &size) == 0 && size >= sizeof(int)) {
            g_isDnDActive.store(val != 0);
        }
    }

    auto pfnSubscribe = reinterpret_cast<PFN_RtlSubscribeWnfStateChangeNotification>(
        GetProcAddress(hNtdll, "RtlSubscribeWnfStateChangeNotification"));
    if (!pfnSubscribe) return;

    pfnSubscribe(&g_wnfDndSubscription, kWnfQuietHoursActiveProfileChanged, 0,
                 WnfDndCallback, nullptr, nullptr, 0, 0);
}

void UnsubscribeDndNotification() {
    if (!g_wnfDndSubscription) return;
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (hNtdll) {
        auto pfnUnsubscribe = reinterpret_cast<PFN_RtlUnsubscribeWnfStateChangeNotification>(
            GetProcAddress(hNtdll, "RtlUnsubscribeWnfStateChangeNotification"));
        if (pfnUnsubscribe) {
            pfnUnsubscribe(g_wnfDndSubscription);
        }
    }
    g_wnfDndSubscription = nullptr;
}


}
