#include "services/github.h"
#include "services/http.h"
#include "core/model.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/util.h"

namespace island
{
namespace
{


// ── GitHub contributions tab (fork addition) ────────────────────────────────
// Reads the public contribution calendar, so no token is needed or stored.
struct GitHubDay {
    std::string date;
    int row = 0;
    int column = 0;
    int level = 0;
    int count = 0;
};

// Pulls the value of attribute `name` from the tag text in [tagStart, tagEnd).
std::string HtmlAttribute(const std::string& html, size_t tagStart, size_t tagEnd, const char* name) {
    const std::string needle = std::string(" ") + name + "=\"";
    const size_t pos = html.find(needle, tagStart);
    if (pos == std::string::npos || pos >= tagEnd) return std::string();
    const size_t begin = pos + needle.size();
    const size_t end = html.find('"', begin);
    if (end == std::string::npos || end > tagEnd) return std::string();
    return html.substr(begin, end - begin);
}

std::vector<GitHubDay> ParseGitHubCalendar(const std::string& html) {
    static const std::string kIdPrefix = "contribution-day-component-";
    std::vector<GitHubDay> days;

    size_t cursor = 0;
    while ((cursor = html.find("<td", cursor)) != std::string::npos) {
        const size_t tagEnd = html.find('>', cursor);
        if (tagEnd == std::string::npos) break;

        const std::string id = HtmlAttribute(html, cursor, tagEnd, "id");
        const std::string date = HtmlAttribute(html, cursor, tagEnd, "data-date");
        const std::string level = HtmlAttribute(html, cursor, tagEnd, "data-level");
        cursor = tagEnd;

        int row = -1, column = -1;
        if (date.size() != 10 || level.empty() || id.rfind(kIdPrefix, 0) != 0 ||
            sscanf(id.c_str() + kIdPrefix.size(), "%d-%d", &row, &column) != 2 || row < 0 || row > 6 ||
            column < 0) {
            continue;
        }
        days.push_back({date, row, column, atoi(level.c_str()), 0});
    }

    // Counts only exist in the tooltips, which refer back to a cell by id.
    std::map<std::pair<int, int>, int> counts;
    cursor = 0;
    while ((cursor = html.find("<tool-tip", cursor)) != std::string::npos) {
        const size_t tagEnd = html.find('>', cursor);
        if (tagEnd == std::string::npos) break;
        const std::string target = HtmlAttribute(html, cursor, tagEnd, "for");
        int row = -1, column = -1;
        if (target.rfind(kIdPrefix, 0) == 0 &&
            sscanf(target.c_str() + kIdPrefix.size(), "%d-%d", &row, &column) == 2) {
            // "No contributions on ..." parses as 0, "3 contributions on ..." as 3.
            counts[{row, column}] = atoi(html.c_str() + tagEnd + 1);
        }
        cursor = tagEnd;
    }
    for (GitHubDay& day : days) {
        const auto found = counts.find({day.row, day.column});
        if (found != counts.end()) day.count = found->second;
    }

    std::sort(days.begin(), days.end(),
              [](const GitHubDay& a, const GitHubDay& b) { return a.date < b.date; });
    return days;
}

void CommitGitHubSnapshot(const std::wstring& user, const std::vector<GitHubDay>& days) {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    char today[16];
    sprintf_s(today, "%04d-%02d-%02d", local.wYear, local.wMonth, local.wDay);

    GitHubSnapshot next;
    next.hasData = true;
    next.user = user;
    next.lastUpdated = NowSeconds();
    next.cells.fill(-1);

    int lastColumn = 0;
    for (const GitHubDay& day : days) {
        lastColumn = std::max(lastColumn, day.column);
        next.yearTotal += day.count;
        if (day.date == today) next.todayCount = day.count;
    }

    const int firstColumn = lastColumn - (GitHubSnapshot::kWeeks - 1);
    for (const GitHubDay& day : days) {
        if (day.column >= firstColumn) {
            next.cells[(day.column - firstColumn) * 7 + day.row] = static_cast<signed char>(day.level);
        }
    }

    // A streak survives until the day is over, so an empty today doesn't break it.
    int index = static_cast<int>(days.size()) - 1;
    if (index >= 0 && days[index].date == today && days[index].count == 0) --index;
    for (; index >= 0 && days[index].count > 0; --index) ++next.streak;

    std::lock_guard lock(g_stateMutex);
    g_state.github = std::move(next);
}

}



DWORD WINAPI GitHubThreadProc(void*) {
    WaitForSingleObject(g_stopEvent, 5000);

    static const DWORD kBackoffMs[] = {60 * 1000, 5 * 60 * 1000, 15 * 60 * 1000, 30 * 60 * 1000};
    size_t backoffIndex = 0;

    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        if (!GetSettingsCopy().githubModule) {
            WaitForSingleObject(g_stopEvent, 5000);
            continue;
        }

        bool committed = false;
        const std::wstring user = GetStringSettingCopy(L"Modules.GitHubUser");
        if (!user.empty()) {
            const std::wstring path = L"/users/" + PercentEncodeUtf8(user) + L"/contributions";
            const std::vector<GitHubDay> days = ParseGitHubCalendar(HttpGet(L"github.com", path.c_str(), true));
            if (!days.empty()) {
                CommitGitHubSnapshot(user, days);
                committed = true;
                host::log(L"GitHub: %zu days for %s", days.size(), user.c_str());
            } else {
                host::log(L"GitHub: no calendar in response, keeping previous data.");
            }
        }

        DWORD waitMs = 30 * 60 * 1000;
        if (committed) {
            backoffIndex = 0;
        } else {
            waitMs = kBackoffMs[backoffIndex];
            if (backoffIndex + 1 < ARRAYSIZE(kBackoffMs)) ++backoffIndex;
        }
        WaitForSingleObject(g_stopEvent, waitMs);
    }
    return 0;
}


}
