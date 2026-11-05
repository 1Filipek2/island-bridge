#include "notifications/notification_center.h"

namespace island
{
namespace notification_center
{
namespace
{

constexpr float hit_slop = 3.0f;

}
}

namespace notification_center
{

int row_at(float x, float y, float width, float height, int row_count)
{
    if (row_count <= 0 || x < pad_x || x > width - pad_x || y < list_top || y > height - list_bottom_inset)
    {
        return -1;
    }

    const float stride = row_height + row_gap;
    const int index = static_cast<int>((y - list_top) / stride);

    if (y > list_top + static_cast<float>(index) * stride + row_height)
    {
        return -1;
    }

    if (index < 0 || index >= visible_rows(height) || index >= row_count)
    {
        return -1;
    }

    return index;
}

bool clear_hit(float x, float y, float width)
{
    const float right = width - pad_x;
    const float left = right - clear_size;
    return x >= left - hit_slop && x <= right + hit_slop && y >= clear_top - hit_slop &&
           y <= clear_top + clear_size + hit_slop;
}

bool remove_hit(float x, float y, float width, int row)
{
    if (row < 0)
    {
        return false;
    }

    const float half = remove_size * 0.5f + hit_slop;
    const float center_x = width - pad_x - remove_margin - remove_size * 0.5f;
    const float center_y = list_top + static_cast<float>(row) * (row_height + row_gap) + row_height * 0.5f;
    return std::fabs(x - center_x) <= half && std::fabs(y - center_y) <= half;
}

bool back_hit(float x, float y, float width)
{
    const float right = width - pad_x - clear_size - 8.0f;
    return x >= pad_x - hit_slop && x <= right && y >= clear_top - hit_slop && y <= clear_top + back_size + hit_slop;
}

std::size_t max_offset(std::size_t entry_count, int capacity)
{
    const std::size_t rows = capacity < 1 ? 1 : static_cast<std::size_t>(capacity);
    return entry_count > rows ? entry_count - rows : 0;
}

std::size_t scrolled(std::size_t offset, int step, std::size_t entry_count, int capacity)
{
    const std::size_t limit = max_offset(entry_count, capacity);
    const std::size_t current = offset < limit ? offset : limit;

    if (step < 0)
    {
        const std::size_t back = static_cast<std::size_t>(-static_cast<long long>(step));
        return current > back ? current - back : 0;
    }

    const std::size_t next = current + static_cast<std::size_t>(step);
    return next < limit ? next : limit;
}

const Entry* entry_at(const Group& group, std::size_t offset, std::size_t row)
{
    const std::size_t index = offset + row;

    if (index >= group.entries.size())
    {
        return nullptr;
    }

    return &group.entries[group.entries.size() - 1 - index];
}

}

}
