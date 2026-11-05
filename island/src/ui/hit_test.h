#pragma once

#include "core/common.h"
#include "core/layout.h"
#include "core/model.h"
#include "notifications/notification_center.h"
namespace island
{

MediaContentPoint MediaContentFromClient(int clientX, int clientY);
int MediaTransportHitTest(const MediaContentPoint& rawPt);
float MediaScrubFractionFromContent(const MediaContentPoint& rawPt);
float MediaScrubFractionUnbounded(const MediaContentPoint& rawPt);
int FileTrayRowAtContentPoint(const MediaContentPoint& pt, int itemCount);
bool FileTrayClearHitTest(const MediaContentPoint& pt);
bool FileTrayPasteHitTest(const MediaContentPoint& pt);
bool FileTrayRemoveHitTest(const MediaContentPoint& pt, int row);

struct NotificationCenterHit {
    int row = -1;
    int action = -1;
};

constexpr int kNotificationCenterBack = -3;
constexpr int kNotificationCenterClear = -2;
const notification_center::Group* OpenNotificationGroup(const SharedState& state);
NotificationCenterHit NotificationCenterHitTest(const MediaContentPoint& pt);
void CloseNotificationCenterDetail();
void HandleNotificationCenterClick(const NotificationCenterHit& hit);
bool ScrollNotificationCenterDetail(int step);
bool MediaArtHitTest(const MediaContentPoint& pt);
bool MediaLyricsButtonHitTest(const MediaContentPoint& rawPt);

// ── Source dock: slot layout + hit testing ──────────────────────────────────
struct DockSlots {
    int count = 0;                      // slots drawn
    int sourceIndex[3] = {-1, -1, -1};  // index into mediaSources; -1 = the "+N" slot
    int overflow = 0;                   // N in "+N" (0 = none)
};
DockSlots ComputeDockSlots(int sourceCount, int activeIndex);

struct DockHit {
    int slot = -1;
    bool overflow = false;  // the "+N" slot
    bool isActive = false;
    std::wstring aumid;
};
DockHit ResolveSourceDockHit(const MediaContentPoint& pt);

}
