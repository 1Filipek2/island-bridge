#include "island_pipe_stubs.h"
#include <cstdio>
#include <random>

namespace
{

int failures = 0;

void check(bool condition, const char* name)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", name);
        ++failures;
    }
}

notification_center::Incoming incoming(const std::wstring& key, const std::wstring& preview)
{
    return notification_center::Incoming{key, key, preview, L"12:00", PixelsRef{}};
}

bool by_sequence(const notification_center::Entry& left, const notification_center::Entry& right)
{
    return left.sequence < right.sequence;
}

bool consistent(const notification_center::History& history)
{
    std::size_t sum = 0;

    for (const auto& group : history.groups())
    {
        if (group.entries.empty() || !std::is_sorted(group.entries.begin(), group.entries.end(), by_sequence))
        {
            return false;
        }

        sum += group.entries.size();
    }

    return sum == history.total() && history.total() <= notification_center::max_notifications && history.groups().size() <= notification_center::max_apps;
}

void grouping()
{
    notification_center::History history;
    history.record(incoming(L"discord", L"a"));
    history.record(incoming(L"mail", L"b"));
    history.record(incoming(L"discord", L"c"));

    check(history.groups().size() == 2, "groups by key");
    check(history.total() == 3, "counts every notification");
    check(history.groups()[0].key == L"discord", "newest app first");
    check(history.groups()[0].entries.size() == 2, "group count");
    check(history.groups()[0].latest().preview == L"c", "latest preview kept");
    check(history.groups()[1].key == L"mail", "older app second");
    check(history.groups()[0].entries[0].preview == L"a", "every message of a group is kept");
}

void finding()
{
    notification_center::History history;
    history.record(incoming(L"a", L"1"));
    history.record(incoming(L"b", L"2"));

    check(history.find(L"a") != nullptr && history.find(L"a")->key == L"a", "find by key");
    check(history.find(L"z") == nullptr, "find misses an unknown key");
    check(history.remove(std::wstring(L"a")), "remove by key");
    check(!history.remove(std::wstring(L"a")), "remove by key twice");
    check(history.groups().size() == 1 && history.total() == 1, "remove by key keeps the rest");
}

void detail_paging()
{
    using namespace notification_center;

    check(max_offset(2, 3) == 0, "short list does not scroll");
    check(max_offset(7, 3) == 4, "long list scrolls to the last page");
    check(max_offset(7, 0) == 6, "capacity floor of one row");
    check(scrolled(0, -1, 7, 3) == 0, "top clamps");
    check(scrolled(0, 1, 7, 3) == 1, "scroll down");
    check(scrolled(4, 1, 7, 3) == 4, "bottom clamps");
    check(scrolled(9, -1, 7, 3) == 3, "stale offset is clamped first");
    check(scrolled(2, -5, 7, 3) == 0, "big step up clamps");

    History history;
    history.record(incoming(L"a", L"1"));
    history.record(incoming(L"a", L"2"));
    history.record(incoming(L"a", L"3"));
    const Group& group = history.groups()[0];

    check(entry_at(group, 0, 0) != nullptr && entry_at(group, 0, 0)->preview == L"3", "newest message first");
    check(entry_at(group, 1, 1) != nullptr && entry_at(group, 1, 1)->preview == L"1", "offset moves to older messages");
    check(entry_at(group, 2, 1) == nullptr, "past the oldest message");

    View view;
    check(!view.open(), "view starts closed");
    view.key = L"a";
    check(view.open(), "view opens with a key");
}

void icon_kept()
{
    notification_center::History history;
    notification_center::Incoming first = incoming(L"discord", L"a");
    first.icon = PixelsRef(BitmapPixels{std::vector<std::uint8_t>(4, 1), 1, 1});
    history.record(std::move(first));
    history.record(incoming(L"discord", L"b"));

    check(static_cast<bool>(history.groups()[0].icon), "icon survives a notification without one");
}

void notification_cap()
{
    notification_center::History history;
    history.record(incoming(L"old", L"x"));

    for (std::size_t i = 0; i < notification_center::max_notifications; ++i)
    {
        history.record(incoming(L"busy", L"y"));
    }

    check(history.total() == notification_center::max_notifications, "notification cap");
    check(history.groups().size() == 1, "oldest app dropped when its last notification goes");
    check(history.groups()[0].key == L"busy", "busy app stays");

    history.record(incoming(L"new", L"z"));
    check(history.total() == notification_center::max_notifications, "cap holds after a new app");
    check(history.groups()[1].entries.size() == notification_center::max_notifications - 1, "oldest of busy dropped");
    check(consistent(history), "consistent after cap");
}

void app_cap()
{
    notification_center::History history;

    for (std::size_t i = 0; i <= notification_center::max_apps; ++i)
    {
        history.record(incoming(L"app" + std::to_wstring(i), L"x"));
        history.record(incoming(L"app" + std::to_wstring(i), L"y"));
    }

    check(history.groups().size() == notification_center::max_apps, "app cap");
    check(history.groups().back().key == L"app1", "least recent app dropped");
    check(history.total() == notification_center::max_apps * 2, "dropped app takes its notifications");
    check(consistent(history), "consistent after app cap");
}

void removing()
{
    notification_center::History history;
    history.record(incoming(L"a", L"1"));
    history.record(incoming(L"b", L"2"));
    history.record(incoming(L"b", L"3"));

    check(!history.remove(5), "remove out of range");
    check(history.remove(0), "remove first row");
    check(history.groups().size() == 1 && history.groups()[0].key == L"a", "other app stays");
    check(history.total() == 1, "total after remove");

    history.clear();
    check(history.groups().empty() && history.total() == 0, "clear empties");

    history.record(incoming(L"a", L"4"));
    check(history.total() == 1, "records after clear");
}

void random_operations()
{
    notification_center::History history;
    std::mt19937 random(7);

    for (int i = 0; i < 200000; ++i)
    {
        const unsigned roll = random() % 100;

        if (roll < 90)
        {
            history.record(incoming(L"app" + std::to_wstring(random() % 20), L"x"));
        }
        else if (roll < 99)
        {
            history.remove(random() % 16);
        }
        else
        {
            history.clear();
        }

        if (!consistent(history))
        {
            check(false, "random operations stay consistent");
            return;
        }
    }
}

void hit_tests()
{
    using namespace notification_center;

    constexpr float width = 380.0f;
    constexpr float height = 184.0f;

    check(visible_rows(height) == 3, "three rows fit");
    check(visible_rows(10.0f) == 1, "at least one row");
    check(row_at(100.0f, list_top + 1.0f, width, height, 5) == 0, "first row");
    check(row_at(100.0f, list_top + row_height + row_gap + 1.0f, width, height, 5) == 1, "second row");
    check(row_at(100.0f, list_top + row_height + 1.0f, width, height, 5) == -1, "gap is not a row");
    check(row_at(100.0f, list_top + 2.0f * (row_height + row_gap) + 1.0f, width, height, 2) == -1, "past the last group");
    check(row_at(100.0f, list_top + 1.0f, width, height, 0) == -1, "empty list");
    check(row_at(pad_x - 1.0f, list_top + 1.0f, width, height, 5) == -1, "left of the list");
    check(row_at(100.0f, height - 1.0f, width, height, 5) == -1, "below the list");
    check(clear_hit(width - pad_x - clear_size * 0.5f, clear_top + clear_size * 0.5f, width), "bin hit");
    check(!clear_hit(width * 0.5f, clear_top + clear_size * 0.5f, width), "bin miss");

    const float remove_x = width - pad_x - remove_margin - remove_size * 0.5f;
    check(remove_hit(remove_x, list_top + row_height * 0.5f, width, 0), "remove hit");
    check(!remove_hit(remove_x, list_top + row_height * 0.5f, width, -1), "remove needs a row");
    check(!remove_hit(100.0f, list_top + row_height * 0.5f, width, 0), "remove miss");
    check(back_hit(pad_x + back_size * 0.5f, clear_top + back_size * 0.5f, width), "back hit");
    check(!back_hit(width - pad_x - clear_size * 0.5f, clear_top + clear_size * 0.5f, width), "bin is not back");
    check(!back_hit(pad_x + back_size * 0.5f, list_top + row_height * 0.5f, width), "row is not back");
}

}

int main()
{
    grouping();
    finding();
    detail_paging();
    icon_kept();
    notification_cap();
    app_cap();
    removing();
    random_operations();
    hit_tests();
    std::printf("notification center tests, failures %d\n", failures);
    return failures == 0 ? 0 : 1;
}
