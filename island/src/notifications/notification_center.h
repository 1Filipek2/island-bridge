#pragma once

#include "core/common.h"
#include "core/pixels.h"

namespace island
{
namespace notification_center
{

constexpr std::size_t max_notifications = 50;
constexpr std::size_t max_apps = 12;
constexpr float pad_x = 26.0f;
constexpr float list_top = 46.0f;
constexpr float list_bottom_inset = 14.0f;
constexpr float row_height = 36.0f;
constexpr float row_gap = 4.0f;
constexpr float clear_size = 24.0f;
constexpr float clear_top = 16.0f;
constexpr float remove_size = 18.0f;
constexpr float remove_margin = 7.0f;
constexpr float unread_dot_radius = 3.0f;
constexpr float unread_dot_gap = 11.0f;
constexpr float back_size = 24.0f;

struct Incoming
{
    std::wstring key;
    std::wstring app;
    std::wstring preview;
    std::wstring time;
    PixelsRef icon;
};

struct Entry
{
    std::uint64_t sequence = 0;
    std::wstring preview;
    std::wstring time;
};

struct Group
{
    std::wstring key;
    std::wstring app;
    PixelsRef icon;
    std::vector<Entry> entries;

    const Entry& latest() const
    {
        return entries.back();
    }
};

struct View
{
    std::wstring key;
    std::size_t offset = 0;

    bool open() const
    {
        return !key.empty();
    }
};

class History
{
public:
    void record(Incoming incoming)
    {
        ++sequence_;

        const auto found = std::find_if(groups_.begin(), groups_.end(),
                                        [&](const Group& group)
                                        {
                                            return group.key == incoming.key;
                                        });

        if (found != groups_.end())
        {
            std::rotate(groups_.begin(), found, found + 1);
        }
        else
        {
            if (groups_.size() >= max_apps)
            {
                total_ -= groups_.back().entries.size();
                groups_.pop_back();
            }

            groups_.reserve(max_apps);
            groups_.insert(groups_.begin(), Group{});
            groups_.front().key = std::move(incoming.key);
        }

        Group& group = groups_.front();
        group.app = std::move(incoming.app);

        if (incoming.icon)
        {
            group.icon = std::move(incoming.icon);
        }

        group.entries.push_back(Entry{sequence_, std::move(incoming.preview), std::move(incoming.time)});
        ++total_;

        while (total_ > max_notifications)
        {
            drop_oldest();
        }
    }

    bool remove(std::size_t index)
    {
        if (index >= groups_.size())
        {
            return false;
        }

        total_ -= groups_[index].entries.size();
        groups_.erase(groups_.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }

    bool remove(const std::wstring& key)
    {
        const auto found = std::find_if(groups_.begin(), groups_.end(),
                                        [&](const Group& group)
                                        {
                                            return group.key == key;
                                        });
        return found != groups_.end() && remove(static_cast<std::size_t>(found - groups_.begin()));
    }

    const Group* find(const std::wstring& key) const
    {
        const auto found = std::find_if(groups_.begin(), groups_.end(),
                                        [&](const Group& group)
                                        {
                                            return group.key == key;
                                        });
        return found == groups_.end() ? nullptr : &*found;
    }

    void clear()
    {
        groups_.clear();
        total_ = 0;
    }

    const std::vector<Group>& groups() const
    {
        return groups_;
    }

    std::size_t total() const
    {
        return total_;
    }

private:
    void drop_oldest()
    {
        const auto oldest = std::min_element(groups_.begin(), groups_.end(),
                                             [](const Group& left, const Group& right)
                                             {
                                                 return left.entries.front().sequence < right.entries.front().sequence;
                                             });

        if (oldest == groups_.end())
        {
            total_ = 0;
            return;
        }

        oldest->entries.erase(oldest->entries.begin());
        --total_;

        if (oldest->entries.empty())
        {
            groups_.erase(oldest);
        }
    }

    std::vector<Group> groups_;
    std::size_t total_ = 0;
    std::uint64_t sequence_ = 0;
};

constexpr int visible_rows(float content_height)
{
    const float span = content_height - list_bottom_inset - list_top + row_gap;
    const int rows = static_cast<int>(span / (row_height + row_gap));
    return rows < 1 ? 1 : rows;
}

int row_at(float x, float y, float width, float height, int row_count);
bool clear_hit(float x, float y, float width);
bool remove_hit(float x, float y, float width, int row);
bool back_hit(float x, float y, float width);
std::size_t max_offset(std::size_t entry_count, int capacity);
std::size_t scrolled(std::size_t offset, int step, std::size_t entry_count, int capacity);
const Entry* entry_at(const Group& group, std::size_t offset, std::size_t row);

}
}
