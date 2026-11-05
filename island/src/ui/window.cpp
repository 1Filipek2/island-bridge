#include "ui/window.h"
#include "ui/actions.h"
#include "ui/backdrop.h"
#include "notifications/capture.h"
#include "core/constants.h"
#include "ui/context_menu.h"
#include "ui/file_tray.h"
#include "ui/hit_test.h"
#include "ui/icons.h"
#include "system/input_hooks.h"
#include "core/layout.h"
#include "media/media_source.h"
#include "core/model.h"
#include "ui/placement.h"
#include "services/quick_lookup.h"
#include "ui/renderer.h"
#include "core/state.h"
#include "core/tabs.h"
#include "core/util.h"

namespace island
{


LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static POINT s_touchStart = {0, 0};
    static ULONGLONG s_touchStartTime = 0;
    static bool s_trayPressActive = false;   // left button went down on a File Tray row
    static POINT s_trayPressPt = {0, 0};
    static std::wstring s_trayPressPath;
    switch (msg) {
        case WM_CREATE:
            AddClipboardFormatListener(hwnd);
            if (g_shellHookMessage == 0) g_shellHookMessage = RegisterWindowMessageW(L"SHELLHOOK");
            if (g_taskbarCreatedMessage == 0) g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
            RegisterShellHookWindow(hwnd);
            // File Tray (#33). An OLE drop target (instead of DragAcceptFiles) so the
            // island can react while a drag is still hovering. The module being off is
            // handled inside TrayDropTarget::DragEnter.
            {
                g_oleInitialized = SUCCEEDED(OleInitialize(nullptr));
                ComPtr<IDropTarget> dropTarget;
                dropTarget.Attach(new TrayDropTarget());
                const HRESULT dropHr = RegisterDragDrop(hwnd, dropTarget.Get());
                if (FAILED(dropHr)) {
                    host::log(L"RegisterDragDrop failed (0x%08X).", static_cast<unsigned>(dropHr));
                }
            }
            return 0;

        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wParam);
            if (!g_settings.fileTrayModule) {
                DragFinish(drop);
                return 0;
            }

            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<FileTrayItem> added;
            added.reserve(count);

            for (UINT i = 0; i < count; ++i) {
                wchar_t path[MAX_PATH] = {};
                if (DragQueryFileW(drop, i, path, ARRAYSIZE(path)) == 0) {
                    continue;
                }

                FileTrayItem item;
                item.path = path;

                // BaseNameFromPath instead of PathFindFileNameW so we do not
                // pull in shlwapi just for this.
                item.name = BaseNameFromPath(path);
                if (item.name.empty()) {
                    item.name = path;
                }

                WIN32_FILE_ATTRIBUTE_DATA attr = {};
                if (GetFileAttributesExW(path, GetFileExInfoStandard, &attr)) {
                    item.isDirectory = (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    item.sizeBytes = (static_cast<uint64_t>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
                }

                // Grab the real Explorer icon so the shelf looks native.
                SHFILEINFOW info = {};
                if (SHGetFileInfoW(path, 0, &info, sizeof(info), SHGFI_ICON | SHGFI_SMALLICON) && info.hIcon) {
                    item.icon = IconToPixelsRef(info.hIcon, 32);
                    DestroyIcon(info.hIcon);
                }

                added.push_back(std::move(item));
            }
            DragFinish(drop);

            if (!added.empty()) {
                std::lock_guard lock(g_stateMutex);
                for (auto& item : added) {
                    // Re-dropping a file promotes the existing entry instead of
                    // creating a duplicate.
                    auto existing = std::find_if(g_state.fileTrayItems.begin(), g_state.fileTrayItems.end(),
                                                 [&](const FileTrayItem& other) {
                                                     return _wcsicmp(other.path.c_str(), item.path.c_str()) == 0;
                                                 });
                    if (existing != g_state.fileTrayItems.end()) {
                        g_state.fileTrayItems.erase(existing);
                    }
                    g_state.fileTrayItems.push_back(std::move(item));
                }

                const size_t cap = static_cast<size_t>(std::max(1, g_settings.fileTrayMaxItems));
                while (g_state.fileTrayItems.size() > cap) {
                    g_state.fileTrayItems.erase(g_state.fileTrayItems.begin());
                }
            }

            // Jump to the shelf so the drop is visibly acknowledged.
            const int trayTab = FileTrayTabIndex(g_settings);
            if (trayTab >= 0) {
                g_idleTab = trayTab;
            }
            g_layoutDirty = true;
            return 0;
        }

        case WM_DESTROY:
            RevokeDragDrop(hwnd);
            if (g_oleInitialized) {
                OleUninitialize();
                g_oleInitialized = false;
            }
            RemoveClipboardFormatListener(hwnd);
            DeregisterShellHookWindow(hwnd);
            return 0;

        // Both of these touch the window from the thread that owns it, the only
        // thread allowed to bind a hot key to it or reshape it.
        case WM_APP_APPLY_HOTKEY:
            ApplyHideShowHotkey();
            return 0;

        case WM_APP_APPLY_BACKDROP:
            ApplyBackdropMaterial(hwnd);
            return 0;

        case WM_APP_MOUSE_WAKE:
            // No-op payload — its only job is to wake MsgWaitForMultipleObjects
            // and get drained by the PeekMessage pump.
            return 0;

        case WM_APP_CAPSLOCK: {
            if (!g_settings.capsLock) return 0;
            bool isNum = (wParam == VK_NUMLOCK);
            bool capsOn = (lParam & 1) != 0;
            bool numOn = (lParam & 2) != 0;
            {
                std::lock_guard lock(g_stateMutex);
                g_state.capsLock.active = true;
                g_state.capsLock.capsOn = capsOn;
                g_state.capsLock.numOn = numOn;
                g_state.capsLock.isNumEvent = isNum;
                g_state.capsLock.expiresAt = NowSeconds() + 2.5;
            }
            TriggerNudge();
            return 0;
        }

        case WM_DEVICECHANGE: {
            // DBT_DEVICEARRIVAL = 0x8000, DBT_DEVICEREMOVECOMPLETE = 0x8004
            if (wParam == 0x8000 || wParam == 0x8004) {
                bool arrived = (wParam == 0x8000);
                std::wstring devName;
                bool isBt = false;

                if (lParam) {
                    auto* hdr = reinterpret_cast<DEV_BROADCAST_HDR*>(lParam);
                    if (hdr->dbch_devicetype == DBT_DEVTYP_VOLUME) {
                        devName = L"USB Drive";
                    } else if (hdr->dbch_devicetype == DBT_DEVTYP_PORT) {
                        devName = L"COM Device";
                    } else {
                        // Generic/Bluetooth OEM
                        isBt = true;
                        devName = L"Bluetooth Device";
                    }
                }

                {
                    std::lock_guard lock(g_stateMutex);
                    g_state.device.active = true;
                    g_state.device.eventType = arrived ? DeviceEventType::Connected
                                                       : DeviceEventType::Disconnected;
                    g_state.device.deviceName = devName;
                    g_state.device.isBluetoothLike = isBt;
                    g_state.device.expiresAt = NowSeconds() + 3.0;
                }
                TriggerNudge();
            }
            return 0;
        }

        case WM_POWERBROADCAST: {
            if (wParam == PBT_APMRESUMESUSPEND || wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMECRITICAL) {
                if (g_settingsChangedEvent) {
                    SetEvent(g_settingsChangedEvent);
                }
                {
                    std::lock_guard lock(g_stateMutex);
                    g_state.weather.lastUpdated = 0.0;
                }
                TriggerNudge();
            }
            return TRUE;
        }

        case WM_CLIPBOARDUPDATE:
            if (g_settings.clipboard) {
                CaptureClipboard(hwnd);
            }
            return 0;



        case WM_NCHITTEST: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);

            // The window is now a big transparent stage, so only the pill itself
            // is clickable; everything else falls through to the windows below.
            if (g_pillClientRect.valid) {
                const PillClientRect& r = g_pillClientRect;
                if (pt.x < r.left || pt.x > r.right || pt.y < r.top || pt.y > r.bottom) {
                    return HTTRANSPARENT;
                }
                return HTCLIENT;
            }

            RECT clientRect = {};
            GetClientRect(hwnd, &clientRect);

            const int minX = static_cast<int>(kRenderPadX);
            const int maxX = clientRect.right - static_cast<int>(kRenderPadX);
            const int minY = (g_settings.notchStyle || g_settings.borderMergedMode) ? 0 : static_cast<int>(kRenderPadY);
            const int maxY = clientRect.bottom - static_cast<int>(kRenderPadY);

            if (maxX > minX && maxY > minY) {
                if (pt.x < minX || pt.x > maxX || pt.y < minY || pt.y > maxY) {
                    return HTTRANSPARENT;
                }
            }
            return HTCLIENT;
        }

        case WM_KEYDOWN:
            if (HandleLookupKeyDown(hwnd, wParam)) return 0;
            break;

        case WM_CHAR:
            if (HandleLookupChar(static_cast<wchar_t>(wParam))) return 0;
            break;

        case WM_KILLFOCUS:
            // Clicked another window / Alt+Tab / Win key: the search is over.
            if (g_lookupUi.open) {
                CloseLookupPanel(hwnd);
            }
            return 0;

        case WM_APP_LAYOUT_CHANGED:
            g_layoutDirty = true;
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                if (g_lookupUi.open) {
                    POINT cp;
                    GetCursorPos(&cp);
                    ScreenToClient(hwnd, &cp);
                    SetCursor(LoadCursorW(nullptr, LookupCursorAt(cp.x, cp.y)));
                    return TRUE;
                }
                if (g_scrubbing.load()) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }

                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);

                bool mediaActive = false;
                {
                    std::lock_guard lock(g_stateMutex);
                    mediaActive = g_settings.media && g_state.media.available;
                }

                const int currentTab = NormalizedTabIndex(g_settings);
                if (mediaActive && currentTab == 0) {
                    const MediaContentPoint cp = MediaContentFromClient(pt.x, pt.y);
                    const bool hoverClickable = MediaArtHitTest(cp) ||
                                               MediaTransportHitTest(cp) != -1 ||
                                               MediaScrubFractionFromContent(cp) >= 0.0f ||
                                               g_hoveredSourceSlot.load(std::memory_order_relaxed) >= 0 ||
                                               (g_settings.mediaTabLyrics && MediaLyricsButtonHitTest(cp));
                    if (hoverClickable) {
                        SetCursor(LoadCursorW(nullptr, IDC_HAND));
                        return TRUE;
                    }
                }

                // A File Tray row under the cursor is clickable (opens the file).
                if (g_settings.fileTrayModule && currentTab == FileTrayTabIndex(g_settings) &&
                    (g_hoveredFileTrayRow.load(std::memory_order_relaxed) >= 0 ||
                     g_hoveredFileTrayAction.load(std::memory_order_relaxed) != -1)) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }

                if (currentTab == NotificationCenterTabIndex(g_settings) &&
                    (g_hoveredNotificationAction.load(std::memory_order_relaxed) != -1 ||
                     g_hoveredNotificationRow.load(std::memory_order_relaxed) >= 0)) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            break;

        case WM_LBUTTONDOWN:
            {
                int xPos = GET_X_LPARAM(lParam);
                int yPos = GET_Y_LPARAM(lParam);

                s_touchStart.x = xPos;
                s_touchStart.y = yPos;
                s_touchStartTime = GetTickCount64();

                if (g_lookupUi.open) {
                    s_trayPressActive = false;
                    return 0;  // handled on button-up by HandleLookupClick
                }

                // File Tray: remember a press on a row so moving the mouse picks it up
                // for dragging, while a plain click still opens it.
                s_trayPressActive = false;
                if (g_settings.fileTrayModule &&
                    NormalizedTabIndex(g_settings) == FileTrayTabIndex(g_settings)) {
                    const MediaContentPoint tcp = MediaContentFromClient(xPos, yPos);
                    std::lock_guard lock(g_stateMutex);
                    const int count = static_cast<int>(g_state.fileTrayItems.size());
                    const int row = FileTrayRowAtContentPoint(tcp, count);
                    if (row >= 0 && !FileTrayRemoveHitTest(tcp, row)) {
                        s_trayPressActive = true;
                        s_trayPressPt.x = xPos;
                        s_trayPressPt.y = yPos;
                        s_trayPressPath =
                            g_state.fileTrayItems[static_cast<size_t>(count - 1 - row)].path;
                    }
                }

                bool mediaActive = false;
                {
                    std::lock_guard lock(g_stateMutex);
                    mediaActive = g_settings.media && g_state.media.available;
                }

                const int currentTab = NormalizedTabIndex(g_settings);
                if (mediaActive && currentTab == 0) {
                    const MediaContentPoint cp = MediaContentFromClient(xPos, yPos);

                    // Source dock: remember the held slot for its press animation.
                    {
                        const DockHit dock = ResolveSourceDockHit(cp);
                        if (dock.slot >= 0) {
                            g_pressedSourceSlot = dock.slot;
                            g_layoutDirty = true;
                            return 0;
                        }
                    }

                    const int cmd = MediaTransportHitTest(cp);
                    if (cmd != -1) {
                        g_pressedMediaButton = cmd;
                        SetCapture(hwnd);
                        g_layoutDirty = true;
                        return 0;
                    }

                    const float fraction = MediaScrubFractionFromContent(cp);
                    if (fraction >= 0.0f) {
                        g_scrubDragFraction.store(fraction, std::memory_order_relaxed);
                        g_scrubbing = true;
                        g_lastLiveSeekTime = 0.0;
                        SetCapture(hwnd);
                        g_layoutDirty = true;
                        return 0;
                    }
                }
            }
            break;

        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme = {};
            tme.cbSize = sizeof(TRACKMOUSEEVENT);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);

            const int xPos = GET_X_LPARAM(lParam);
            const int yPos = GET_Y_LPARAM(lParam);

            if (g_lookupUi.open) {
                HandleLookupMouseMove(xPos, yPos);
                return 0;
            }

            if (g_scrubbing.load()) {
                const MediaContentPoint cp = MediaContentFromClient(xPos, yPos);
                const float fraction = MediaScrubFractionUnbounded(cp);
                if (fraction < 0.0f) {
                    return 0;
                }
                g_scrubDragFraction.store(fraction, std::memory_order_relaxed);
                g_layoutDirty = true;

                const double now = NowSeconds();
                if (now - g_lastLiveSeekTime.load(std::memory_order_relaxed) >= 0.15) {
                    g_lastLiveSeekTime.store(now, std::memory_order_relaxed);
                    int64_t endTicks = 0;
                    {
                        std::lock_guard lock(g_stateMutex);
                        endTicks = g_state.media.endTicks;
                    }
                    if (endTicks > 0) {
                        SeekMediaToTicks(static_cast<int64_t>(fraction * endTicks));
                    }
                }
                return 0;
            }

            int hovered = -1;
            bool mediaActive = false;
            {
                std::lock_guard lock(g_stateMutex);
                mediaActive = g_settings.media && g_state.media.available;
            }
            const int currentTab = NormalizedTabIndex(g_settings);

            int hoveredSlot = -1;
            if (mediaActive && currentTab == 0) {
                const MediaContentPoint mediaPt = MediaContentFromClient(xPos, yPos);
                hoveredSlot = ResolveSourceDockHit(mediaPt).slot;
                // The dock's right edge overlaps the prev button's slop; the dock wins.
                hovered = hoveredSlot >= 0 ? -1 : MediaTransportHitTest(mediaPt);
            }

            if (g_hoveredMediaButton.exchange(hovered) != hovered) {
                g_layoutDirty = true;
            }
            if (g_hoveredSourceSlot.exchange(hoveredSlot) != hoveredSlot) {
                g_layoutDirty = true;
            }
            {
                bool onLyricsBtn = false;
                if (g_settings.mediaTabLyrics && mediaActive && currentTab == 0) {
                    onLyricsBtn = MediaLyricsButtonHitTest(MediaContentFromClient(xPos, yPos));
                }
                if (g_hoveredLyricsBtn.exchange(onLyricsBtn) != onLyricsBtn) {
                    g_layoutDirty = true;
                }
            }

            // File Tray hover: row highlight plus the bin / cut buttons.
            int hoveredRow = -1;
            int hoveredAction = -1;
            if (g_settings.fileTrayModule && currentTab == FileTrayTabIndex(g_settings)) {
                size_t trayCount = 0;
                {
                    std::lock_guard lock(g_stateMutex);
                    trayCount = g_state.fileTrayItems.size();
                }
                const MediaContentPoint trayPt = MediaContentFromClient(xPos, yPos);
                hoveredRow = FileTrayRowAtContentPoint(trayPt, static_cast<int>(trayCount));
                if (trayCount > 0 && FileTrayClearHitTest(trayPt)) {
                    hoveredAction = -2;
                } else if (FileTrayPasteHitTest(trayPt)) {
                    hoveredAction = -3;
                } else if (hoveredRow >= 0 && FileTrayRemoveHitTest(trayPt, hoveredRow)) {
                    hoveredAction = hoveredRow;
                }
            }
            bool trayHoverChanged = false;
            if (g_hoveredFileTrayRow.exchange(hoveredRow) != hoveredRow) {
                trayHoverChanged = true;
            }
            if (g_hoveredFileTrayAction.exchange(hoveredAction) != hoveredAction) {
                trayHoverChanged = true;
            }
            if (trayHoverChanged) {
                g_layoutDirty = true;
            }

            NotificationCenterHit centerHit;
            if (currentTab == NotificationCenterTabIndex(g_settings)) {
                centerHit = NotificationCenterHitTest(MediaContentFromClient(xPos, yPos));
            }
            const bool centerRowChanged = g_hoveredNotificationRow.exchange(centerHit.row) != centerHit.row;
            const bool centerActionChanged =
                g_hoveredNotificationAction.exchange(centerHit.action) != centerHit.action;
            if (centerRowChanged || centerActionChanged) {
                g_layoutDirty = true;
            }

            // Pressed on a row and moved past the system drag threshold: pick the file up.
            if (s_trayPressActive) {
                if (!(wParam & MK_LBUTTON)) {
                    s_trayPressActive = false;
                } else if (abs(xPos - s_trayPressPt.x) >= GetSystemMetrics(SM_CXDRAG) ||
                           abs(yPos - s_trayPressPt.y) >= GetSystemMetrics(SM_CYDRAG)) {
                    s_trayPressActive = false;
                    const std::wstring dragPath = s_trayPressPath;
                    StartFileTrayDrag(dragPath);
                    return 0;
                }
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            if (g_hoveredSourceSlot.exchange(-1) != -1 || g_pressedSourceSlot.exchange(-1) != -1) {
                g_layoutDirty = true;
            }
            if (g_hoveredMediaButton.exchange(-1) != -1) {
                g_layoutDirty = true;
            }
            if (g_hoveredLyricsBtn.exchange(false)) {
                g_layoutDirty = true;
            }
            s_trayPressActive = false;
            if (g_hoveredFileTrayRow.exchange(-1) != -1) {
                g_layoutDirty = true;
            }
            if (g_hoveredFileTrayAction.exchange(-1) != -1) {
                g_layoutDirty = true;
            }
            if (g_hoveredNotificationRow.exchange(-1) != -1) {
                g_layoutDirty = true;
            }
            if (g_hoveredNotificationAction.exchange(-1) != -1) {
                g_layoutDirty = true;
            }
            return 0;

        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lParam) != hwnd) {
                if (g_scrubbing.exchange(false)) {
                    g_layoutDirty = true;
                }
                if (g_pressedMediaButton.exchange(-1) != -1) {
                    g_layoutDirty = true;
                }
                if (g_hoveredMediaButton.exchange(-1) != -1) {
                    g_layoutDirty = true;
                }
            }
            return 0;

        case WM_LBUTTONUP:
            {
                if (g_scrubbing.load()) {
                    g_scrubbing = false;
                    ReleaseCapture();

                    const float finalFraction = Clamp(g_scrubDragFraction.load(std::memory_order_relaxed), 0.0f, 1.0f);
                    int64_t endTicks = 0;
                    {
                        std::lock_guard lock(g_stateMutex);
                        endTicks = g_state.media.endTicks;
                    }
                    if (endTicks > 0) {
                        const int64_t targetTicks = static_cast<int64_t>(finalFraction * endTicks);
                        SeekMediaToTicks(targetTicks);
                        std::lock_guard lock(g_stateMutex);
                        g_state.media.positionTicks = targetTicks;
                        g_state.media.lastUpdatedTicks = GetTickCount64();
                    }
                    g_layoutDirty = true;
                    return 0;
                }

                if (g_pressedMediaButton.load() != -1) {
                    g_pressedMediaButton = -1;
                    ReleaseCapture();
                    g_layoutDirty = true;
                }
                if (g_pressedSourceSlot.exchange(-1) != -1) {
                    g_layoutDirty = true;
                }

                int xPos = GET_X_LPARAM(lParam);
                int yPos = GET_Y_LPARAM(lParam);

                ULONGLONG now = GetTickCount64();
                if (s_touchStartTime > 0 && (now - s_touchStartTime) < 500) {
                    const int dx = xPos - s_touchStart.x;
                    const int dy = yPos - s_touchStart.y;
                    const int travel = abs(dy) > abs(dx) ? dy : dx;  // dominant axis wins
                    if (abs(travel) > 40) {
                        // Swipe up / left -> next tab, down / right -> previous tab
                        if (ActiveTabCount(GetSettingsCopy()) > 1) {
                            StepIdleTab(travel > 0 ? -1 : 1);
                        }
                        s_touchStartTime = 0;
                        return 0; // Consume swipe gesture
                    }
                }
                s_touchStartTime = 0;

                bool mediaActive = false;
                if (HandleLookupClick(xPos, yPos)) {
                    return 0;
                }
                std::vector<IslandKind> kinds;
                {
                    std::lock_guard lock(g_stateMutex);
                    mediaActive = g_settings.media && g_state.media.available;
                    kinds = ChooseActivities(g_state, g_settings, NowSeconds());
                }

                // Clicking an active transient notification or clipboard card immediately dismisses it cleanly.
                // In a split layout (kinds.size() > 1), don't dismiss if clicking on the secondary pill on the right.
                const bool onSecondarySplit = (kinds.size() > 1 && g_pillClientRect.valid &&
                                               xPos > (g_pillClientRect.left + (g_pillClientRect.right - g_pillClientRect.left) * 0.5f));
                if (!kinds.empty() && !onSecondarySplit &&
                    (kinds[0] == IslandKind::Notification || kinds[0] == IslandKind::Clipboard)) {
                    {
                        std::lock_guard lock(g_stateMutex);
                        if (kinds[0] == IslandKind::Notification) {
                            g_state.notification.active = false;
                            g_state.notification.expiresAt = 0.0;
                        } else if (kinds[0] == IslandKind::Clipboard) {
                            g_state.clipboard.active = false;
                            g_state.clipboard.expiresAt = 0.0;
                        }
                    }
                    g_clickExpanded = false;
                    g_layoutDirty = true;
                    TriggerNudge();
                    return 0;
                }
                const bool gameMetricsPresent =
                    !kinds.empty() && kinds[0] == IslandKind::Idle &&
                    (g_settings.gameOverlay || g_gameOverlayPinned.load());

                bool expanded = g_pinnedExpanded.load() || g_clickExpanded.load();
                if (!gameMetricsPresent && !g_settings.expandOnHover && !expanded) {
                    g_clickExpanded = true;
                    g_layoutDirty = true;
                    return 0; // consumed click to expand
                }

                RECT clientRect;
                GetClientRect(hwnd, &clientRect);
                const float height = static_cast<float>(clientRect.bottom - clientRect.top);

                const int currentTab = NormalizedTabIndex(g_settings);

                const MediaContentPoint cp = MediaContentFromClient(xPos, yPos);

                if (currentTab == NotificationCenterTabIndex(g_settings)) {
                    const NotificationCenterHit hit = NotificationCenterHitTest(cp);
                    if (hit.action != -1 || hit.row >= 0) {
                        HandleNotificationCenterClick(hit);
                        g_hoveredNotificationRow = -1;
                        g_hoveredNotificationAction = -1;
                        g_layoutDirty = true;
                        return 0;
                    }
                }

                // File Tray: the bin clears the shelf, the cut button removes one file,
                // and a plain click on a row opens it with its default app. (Dragging a
                // row out is started from WM_MOUSEMOVE.)
                if (g_settings.fileTrayModule && currentTab == FileTrayTabIndex(g_settings)) {
                    const bool wasPressed = s_trayPressActive;
                    s_trayPressActive = false;

                    if (FileTrayPasteHitTest(cp)) {
                        PasteIntoFileTray(hwnd);
                        g_layoutDirty = true;
                        return 0;
                    }

                    std::wstring toOpen;
                    std::wstring toCopy;
                    bool changed = false;
                    {
                        std::lock_guard lock(g_stateMutex);
                        const int count = static_cast<int>(g_state.fileTrayItems.size());
                        if (count > 0 && FileTrayClearHitTest(cp)) {
                            g_state.fileTrayItems.clear();
                            changed = true;
                        } else {
                            const int row = FileTrayRowAtContentPoint(cp, count);
                            if (row >= 0) {
                                // Rows render newest-first, so map back from the end.
                                const size_t index = static_cast<size_t>(count - 1 - row);
                                if (FileTrayRemoveHitTest(cp, row)) {
                                    g_state.fileTrayItems.erase(
                                        g_state.fileTrayItems.begin() + static_cast<std::ptrdiff_t>(index));
                                    changed = true;
                                } else if (wasPressed) {
                                    const FileTrayItem& clicked = g_state.fileTrayItems[index];
                                    if (clicked.isText) {
                                        toCopy = clicked.text;
                                    } else {
                                        toOpen = clicked.path;
                                    }
                                }
                            }
                        }
                    }
                    if (changed) {
                        SaveFileTray();
                        g_hoveredFileTrayRow = -1;
                        g_hoveredFileTrayAction = -1;
                        g_layoutDirty = true;
                        return 0;
                    }
                    if (!toCopy.empty()) {
                        CopyTextToClipboard(hwnd, toCopy);
                        return 0;
                    }
                    if (!toOpen.empty()) {
                        SHELLEXECUTEINFOW sei = {sizeof(sei)};
                        sei.fMask = SEE_MASK_FLAG_NO_UI;
                        sei.lpFile = toOpen.c_str();
                        sei.nShow = SW_SHOWNORMAL;
                        ShellExecuteExW(&sei);
                        return 0;
                    }
                    // A click on the empty shelf should not fall through to
                    // "focus the media app".
                    return 0;
                }

                if (mediaActive && currentTab == 0) {
                    // Source dock: "+N" opens the list, another slot switches to it.
                    {
                        const DockHit dock = ResolveSourceDockHit(cp);
                        if (dock.slot >= 0) {
                            if (dock.overflow) {
                                POINT sp = {xPos, yPos};
                                ClientToScreen(hwnd, &sp);
                                ShowMediaSourcePopup(hwnd, sp);
                            } else if (!dock.isActive) {
                                RequestMediaSourceSwitch(dock.aumid);
                            }
                            g_layoutDirty = true;
                            return 0;
                        }
                    }

                    if (g_settings.mediaTabLyrics && MediaLyricsButtonHitTest(cp)) {
                        g_immersiveLyrics.store(!g_immersiveLyrics.load());
                        g_layoutDirty = true;
                        return 0;
                    }

                    const int cmd = MediaTransportHitTest(cp);
                    if (cmd != -1) {
                        if (cmd == 0) g_skipTriggerPrev.fetch_add(1);
                        else if (cmd == 2) g_skipTriggerNext.fetch_add(1);
                        g_layoutDirty = true;
                        SendMediaTransportCommand(cmd);
                        return 0;
                    }

                    // A release over the scrubber that never went through the
                    // drag path (press started elsewhere) is swallowed rather
                    // than falling through to "open the app". The actual seek
                    // is handled by the g_scrubbing branch above.
                    if (MediaScrubFractionFromContent(cp) >= 0.0f) {
                        return 0;
                    }
                }

                if (height > 50.0f) {
                    if (mediaActive) {
                        if (currentTab == 0) {
                            OpenRelevantApp();
                        }
                    } else if (kinds.empty() || kinds[0] != IslandKind::Idle) {
                        HandleStatusClickAtPoint(hwnd, lParam);
                    }
                } else {
                    if (mediaActive) {
                        OpenRelevantApp();
                    } else {
                        HandleStatusClickAtPoint(hwnd, lParam);
                    }
                }
            }
            return 0;

        case WM_MBUTTONUP:
            ToggleEndpointMute();
            return 0;

        case WM_LBUTTONDBLCLK: {
            const bool nextVal = !g_pinnedExpanded.load();
            g_pinnedExpanded.store(nextVal);
            host::set_int_value(L"PinnedExpanded", nextVal ? 1 : 0);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            // Delta is accumulated rather than dropped inside a debounce window, so
            // every notch counts and touchpads (many small deltas) work properly.
            // The spring is interruptible, so a step never has to wait for the
            // previous one to finish.
            static int s_wheelAccum = 0;
            static ULONGLONG s_lastWheelEvent = 0;
            static ULONGLONG s_lastWheelStep = 0;

            const ULONGLONG wheelNow = GetTickCount64();
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (delta == 0) return 0;

            // New gesture (pause, or direction change): forget the leftover.
            if (wheelNow - s_lastWheelEvent > 220 ||
                (s_wheelAccum != 0 && ((delta > 0) != (s_wheelAccum > 0)))) {
                s_wheelAccum = 0;
            }
            s_lastWheelEvent = wheelNow;
            s_wheelAccum += delta;

            if (abs(s_wheelAccum) < WHEEL_DELTA) return 0;

            // Rate limit so a free-spinning wheel or touchpad inertia can't race
            // through every tab. At most one notch is held pending, never a backlog.
            const bool precise = abs(delta) < WHEEL_DELTA;
            const ULONGLONG cooldown = precise ? 200 : 85;
            if (wheelNow - s_lastWheelStep < cooldown) {
                s_wheelAccum = s_wheelAccum > 0 ? WHEEL_DELTA : -WHEEL_DELTA;
                return 0;
            }
            s_lastWheelStep = wheelNow;
            const int wheelStep = s_wheelAccum > 0 ? -1 : 1;
            const bool detailScrolled = NormalizedTabIndex(g_settings) == NotificationCenterTabIndex(g_settings) &&
                                        ScrollNotificationCenterDetail(wheelStep);
            if (!detailScrolled) {
                StepIdleTab(wheelStep);  // wheel up = previous tab
            }
            s_wheelAccum = 0;
            return 0;
        }

        case WM_RBUTTONUP: {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd, &pt);
            ShowContextMenu(hwnd, pt);
            return 0;
        }

        case WM_DPICHANGED:
        case WM_DISPLAYCHANGE: {
            g_layoutDirty = true;
            PostMessageW(hwnd, WM_APP_NEW_EVENT, 0, 0);
            return 0;
        }

        case WM_HOTKEY: {
            if (wParam == ID_LOOKUP_HOTKEY) {
                StartQuickLookup(hwnd);
                return 0;
            }
            if (wParam == ID_HIDE_SHOW_HOTKEY) {
                if (g_isFullscreen.load(std::memory_order_relaxed)) {
                    g_fullscreenOverrideVisible = !g_fullscreenOverrideVisible.load();
                    g_autoHiddenParked = false;
                    g_layoutDirty = true;
                    if (g_fullscreenOverrideVisible.load()) {
                        g_manuallyHidden = false;
                        host::set_int_value(L"ManuallyHidden", 0);
                        g_hotkeyUnhideUntil.store(NowSeconds() + (g_settings.autoHideIdleSeconds > 0 ? g_settings.autoHideIdleSeconds : 6.0));
                        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                        PostMessageW(hwnd, WM_APP_NEW_EVENT, 0, 0);
                    }
                } else {
                    bool isCurrentlyHidden = g_manuallyHidden.load() || g_autoHiddenParked.load();
                    if (hwnd && !IsWindowVisible(hwnd)) {
                        isCurrentlyHidden = true;
                    }
                    if (isCurrentlyHidden) {
                        // User wants to reveal / unhide
                        g_manuallyHidden = false;
                        host::set_int_value(L"ManuallyHidden", 0);
                        g_autoHiddenParked = false;
                        g_fullscreenOverrideVisible = true;
                        g_hotkeyUnhideUntil.store(NowSeconds() + (g_settings.autoHideIdleSeconds > 0 ? g_settings.autoHideIdleSeconds : 6.0));
                        g_layoutDirty = true;
                        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                        PostMessageW(hwnd, WM_APP_NEW_EVENT, 0, 0);
                    } else {
                        // User wants to manually hide
                        g_manuallyHidden = true;
                        host::set_int_value(L"ManuallyHidden", 1);
                        g_hotkeyUnhideUntil.store(0.0);
                        g_fullscreenOverrideVisible = false;
                        g_layoutDirty = true;
                        PostMessageW(hwnd, WM_APP_NEW_EVENT, 0, 0);
                    }
                }
            }
            return 0;
        }
    }

    if (msg == g_shellHookMessage && g_shellHookMessage != 0) {
        if (wParam == HSHELL_WINDOWCREATED) {
            CaptureShellNotification(reinterpret_cast<HWND>(lParam));
        }
        return 0;
    }

    if (msg == g_taskbarCreatedMessage && g_taskbarCreatedMessage != 0) {
        host::log(L"TaskbarCreated received; re-registering shell hook window.");
        DeregisterShellHookWindow(hwnd);
        RegisterShellHookWindow(hwnd);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}


}
