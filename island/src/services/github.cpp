#include "services/github.h"
#include "services/http.h"
#include "core/model.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/util.h"

#include <charconv>
#include <string_view>

namespace island
{
namespace
{
bool parse_cell(std::string_view text, int& row, int& column)
{
    const char* const end = text.data() + text.size();
    const auto first = std::from_chars(text.data(), end, row);

    if (first.ec != std::errc{} || first.ptr == end || *first.ptr != '-')
    {
        return false;
    }

    return std::from_chars(first.ptr + 1, end, column).ec == std::errc{};
}

// GitHub contributions tab: reads the public calendar, no token.
struct GitHubDay
{
    std::string date;
    int row = 0;
    int column = 0;
    int level = 0;
    int count = 0;
};

std::string html_attribute(const std::string& html, std::size_t tag_start, std::size_t tag_end, const char* name)
{
    const std::string needle = std::string(" ") + name + "=\"";
    const std::size_t pos = html.find(needle, tag_start);
    if (pos == std::string::npos || pos >= tag_end)
    {
        return std::string();
    }
    const std::size_t begin = pos + needle.size();
    const std::size_t end = html.find('"', begin);
    if (end == std::string::npos || end > tag_end)
    {
        return std::string();
    }
    return html.substr(begin, end - begin);
}

std::vector<GitHubDay> parse_git_hub_calendar(const std::string& html)
{
    static const std::string id_prefix = "contribution-day-component-";
    std::vector<GitHubDay> days;

    std::size_t cursor = 0;
    while ((cursor = html.find("<td", cursor)) != std::string::npos)
    {
        const std::size_t tag_end = html.find('>', cursor);
        if (tag_end == std::string::npos)
        {
            break;
        }

        const std::string id = html_attribute(html, cursor, tag_end, "id");
        const std::string date = html_attribute(html, cursor, tag_end, "data-date");
        const std::string level = html_attribute(html, cursor, tag_end, "data-level");
        cursor = tag_end;

        int row = -1, column = -1;
        if (date.size() != 10 || level.empty() || id.rfind(id_prefix, 0) != 0 ||
            !parse_cell(std::string_view(id).substr(id_prefix.size()), row, column) || row < 0 || row > 6 || column < 0)
        {
            continue;
        }
        days.push_back({date, row, column, std::atoi(level.c_str()), 0});
    }

    // Counts only exist in the tooltips, which refer back to a cell by id.
    std::map<std::pair<int, int>, int> counts;
    cursor = 0;
    while ((cursor = html.find("<tool-tip", cursor)) != std::string::npos)
    {
        const std::size_t tag_end = html.find('>', cursor);
        if (tag_end == std::string::npos)
        {
            break;
        }
        const std::string target = html_attribute(html, cursor, tag_end, "for");
        int row = -1, column = -1;
        if (target.rfind(id_prefix, 0) == 0 &&
            parse_cell(std::string_view(target).substr(id_prefix.size()), row, column))
        {
            counts[{row, column}] = std::atoi(html.c_str() + tag_end + 1);
        }
        cursor = tag_end;
    }
    for (GitHubDay& day : days)
    {
        const auto found = counts.find({day.row, day.column});
        if (found != counts.end())
        {
            day.count = found->second;
        }
    }

    std::sort(days.begin(), days.end(),
              [](const GitHubDay& a, const GitHubDay& b)
              {
                  return a.date < b.date;
              });
    return days;
}

void commit_git_hub_snapshot(const std::wstring& user, const std::vector<GitHubDay>& days)
{
    SYSTEMTIME local{};
    GetLocalTime(&local);
    char today[16];
    sprintf_s(today, "%04d-%02d-%02d", local.wYear, local.wMonth, local.wDay);

    GitHubSnapshot next;
    next.has_data = true;
    next.user = user;
    next.last_updated = now_seconds();
    next.cells.fill(-1);

    int last_column = 0;
    for (const GitHubDay& day : days)
    {
        last_column = std::max(last_column, day.column);
        next.year_total += day.count;
        if (day.date == today)
        {
            next.today_count = day.count;
        }
    }

    const int first_column = last_column - (GitHubSnapshot::weeks - 1);
    for (const GitHubDay& day : days)
    {
        if (day.column >= first_column)
        {
            next.cells[(day.column - first_column) * 7 + day.row] = static_cast<signed char>(day.level);
        }
    }

    // A streak survives until the day is over, so an empty today doesn't break it.
    int index = static_cast<int>(days.size()) - 1;
    if (index >= 0 && days[index].date == today && days[index].count == 0)
    {
        --index;
    }
    for (; index >= 0 && days[index].count > 0; --index)
    {
        ++next.streak;
    }

    std::lock_guard lock(g_state_mutex);
    g_state.github = std::move(next);
}

}

DWORD WINAPI git_hub_thread_proc(void*)
{
    WaitForSingleObject(g_stop_event, 5000);

    static const DWORD backoff_ms[] = {60 * 1000, 5 * 60 * 1000, 15 * 60 * 1000, 30 * 60 * 1000};
    std::size_t backoff_index = 0;

    while (WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT)
    {
        if (!get_settings_copy().github_module)
        {
            WaitForSingleObject(g_stop_event, 5000);
            continue;
        }

        bool committed = false;
        const std::wstring user = get_string_setting_copy(L"Modules.GitHubUser");
        if (!user.empty())
        {
            const std::wstring path = L"/users/" + percent_encode_utf8(user) + L"/contributions";
            const std::vector<GitHubDay> days = parse_git_hub_calendar(http_get(L"github.com", path.c_str(), true));
            if (!days.empty())
            {
                commit_git_hub_snapshot(user, days);
                committed = true;
                host::log(L"GitHub: %zu days for %s", days.size(), user.c_str());
            }
            else
            {
                host::log(L"GitHub: no calendar in response, keeping previous data.");
            }
        }

        DWORD wait_ms = 30 * 60 * 1000;
        if (committed)
        {
            backoff_index = 0;
        }
        else
        {
            wait_ms = backoff_ms[backoff_index];
            if (backoff_index + 1 < ARRAYSIZE(backoff_ms))
            {
                ++backoff_index;
            }
        }
        WaitForSingleObject(g_stop_event, wait_ms);
    }
    return 0;
}

}
