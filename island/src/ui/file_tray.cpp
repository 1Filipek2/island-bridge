#include "ui/file_tray.h"
#include "notifications/capture.h"
#include "ui/icons.h"
#include "media/lyrics_cache.h"
#include "core/model.h"

namespace island
{
namespace file_tray_store
{
namespace
{

inline std::wstring get_current_user_sid()
{
    HANDLE h_token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &h_token))
    {
        DWORD len = 0;
        GetTokenInformation(h_token, TokenUser, nullptr, 0, &len);
        if (len > 0)
        {
            std::vector<BYTE> buffer(len);
            if (GetTokenInformation(h_token, TokenUser, buffer.data(), len, &len))
            {
                auto* token_user = reinterpret_cast<TOKEN_USER*>(buffer.data());
                LPWSTR sid_string = nullptr;
                if (ConvertSidToStringSidW(token_user->User.Sid, &sid_string))
                {
                    std::wstring result = sid_string;
                    LocalFree(sid_string);
                    CloseHandle(h_token);
                    return result;
                }
            }
        }
        CloseHandle(h_token);
    }
    wchar_t user_buf[256] = {};
    DWORD un = ARRAYSIZE(user_buf);
    if (GetUserNameW(user_buf, &un) && un > 0)
    {
        return user_buf;
    }
    return L"default";
}

}
}

namespace file_tray_store
{

bool resolve_paths(std::wstring* dir, std::wstring* file)
{
    wchar_t storage_path[MAX_PATH] = {};
    if (!host::storage_path(storage_path, ARRAYSIZE(storage_path)))
    {
        return false;
    }
    const std::wstring user_key = get_current_user_sid();
    *dir = std::wstring(storage_path) + L"\\filetray\\" + user_key;
    *file = *dir + L"\\items.dat";
    return true;
}

std::string escape(const std::wstring& value)
{
    std::string out;
    for (const char c : lyrics_cache::wide_to_utf8(value))
    {
        switch (c)
        {
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

std::wstring unescape(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i)
    {
        if (value[i] == '\\' && i + 1 < value.size())
        {
            const char next = value[++i];
            switch (next)
            {
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                default:
                    out.push_back(next);
                    break;
            }
        }
        else
        {
            out.push_back(value[i]);
        }
    }
    return lyrics_cache::utf8_to_wide(out);
}

}

// Writes the current tray to disk. Safe to call from any thread, and never call
// it while holding g_stateMutex. With persistence off, or an empty tray, the
// saved file is removed instead.
void save_file_tray()
{
    bool persist = true;
    {
        std::lock_guard lock(g_settings_mutex);
        persist = g_settings.file_tray_persist;
    }

    struct Row
    {
        bool is_text;
        std::wstring name;
        std::wstring value;
    };

    std::vector<Row> rows;
    {
        std::lock_guard lock(g_state_mutex);
        rows.reserve(g_state.file_tray_items.size());
        for (const FileTrayItem& item : g_state.file_tray_items)
        {
            rows.push_back(Row{item.is_text, item.name, item.is_text ? item.text : item.path});
        }
    }

    std::lock_guard io(file_tray_store::g_io_mutex);
    std::wstring dir, file;
    if (!file_tray_store::resolve_paths(&dir, &file))
    {
        return;
    }

    if (!persist || rows.empty())
    {
        DeleteFileW(file.c_str());
        return;
    }

    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);

    std::string data = "DIWFT 1\n";
    for (const Row& row : rows)
    {
        if (row.is_text)
        {
            data += "T\t" + file_tray_store::escape(row.name) + "\t" + file_tray_store::escape(row.value) + "\n";
        }
        else
        {
            data += "F\t" + file_tray_store::escape(row.value) + "\n";
        }
    }

    if (!lyrics_cache::write_file_atomic(file, data))
    {
        host::log(L"File Tray: could not save %s (error %lu).", file.c_str(), GetLastError());
    }
}

// Reads the saved tray back. Runs once on the render thread at startup (it needs
// COM for the shell icons). Files that have since moved or been deleted are kept,
// since you asked for items to stay until you remove them; they just show a
// generic icon.
void load_file_tray()
{
    bool persist = true;
    int max_items = 10;
    {
        std::lock_guard lock(g_settings_mutex);
        persist = g_settings.file_tray_persist;
        max_items = std::max(1, g_settings.file_tray_max_items);
    }

    std::lock_guard io(file_tray_store::g_io_mutex);
    std::wstring dir, file;
    if (!file_tray_store::resolve_paths(&dir, &file))
    {
        return;
    }
    if (!persist)
    {
        DeleteFileW(file.c_str());
        return;
    }

    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        return;
    }

    constexpr LONGLONG max_bytes = 16ll * 1024 * 1024;
    std::string data;
    LARGE_INTEGER size = {};
    bool read_ok = GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart <= max_bytes;
    if (read_ok)
    {
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        read_ok = ReadFile(h, data.data(), static_cast<DWORD>(size.QuadPart), &got, nullptr) &&
                  got == static_cast<DWORD>(size.QuadPart);
    }
    CloseHandle(h);

    static const char k_header[] = "DIWFT 1\n";
    if (!read_ok || data.compare(0, sizeof(k_header) - 1, k_header) != 0)
    {
        host::log(L"File Tray: saved list is unreadable; starting empty.");
        return;
    }

    std::vector<FileTrayItem> loaded;
    size_t pos = sizeof(k_header) - 1;
    while (pos < data.size())
    {
        size_t end = data.find('\n', pos);
        if (end == std::string::npos)
        {
            end = data.size();
        }
        const std::string line = data.substr(pos, end - pos);
        pos = end + 1;

        if (line.size() < 3 || line[1] != '\t')
        {
            continue;
        }

        FileTrayItem item;
        if (line[0] == 'T')
        {
            const std::string rest = line.substr(2);
            const size_t tab = rest.find('\t');
            if (tab == std::string::npos)
            {
                continue;
            }
            item.is_text = true;
            item.name = file_tray_store::unescape(rest.substr(0, tab));
            item.text = file_tray_store::unescape(rest.substr(tab + 1));
            if (item.text.empty())
            {
                continue;
            }
            if (item.name.empty())
            {
                item.name = L"Text";
            }
        }
        else if (line[0] == 'F')
        {
            item.path = file_tray_store::unescape(line.substr(2));
            if (item.path.empty())
            {
                continue;
            }
            item.name = base_name_from_path(item.path);
            if (item.name.empty())
            {
                item.name = item.path;
            }

            WIN32_FILE_ATTRIBUTE_DATA attr = {};
            const bool exists = GetFileAttributesExW(item.path.c_str(), GetFileExInfoStandard, &attr) != 0;
            if (exists)
            {
                item.is_directory = (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                item.size_bytes = (static_cast<uint64_t>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
            }

            // A missing file gets its type icon from the extension alone, which
            // never touches the disk (so an unplugged drive can't stall startup).
            SHFILEINFOW info = {};
            UINT flags = SHGFI_ICON | SHGFI_LARGEICON;
            if (!exists)
            {
                flags |= SHGFI_USEFILEATTRIBUTES;
            }
            if (SHGetFileInfoW(item.path.c_str(), exists ? 0 : FILE_ATTRIBUTE_NORMAL, &info, sizeof(info), flags) &&
                info.hIcon)
            {
                item.icon = icon_to_pixels_ref(info.hIcon, 32);
                DestroyIcon(info.hIcon);
            }
        }
        else
        {
            continue;
        }
        loaded.push_back(std::move(item));
    }

    if (loaded.empty())
    {
        return;
    }

    std::lock_guard lock(g_state_mutex);
    auto& items = g_state.file_tray_items;
    // Saved items are older than anything dropped since startup, so they go in front.
    items.insert(items.begin(), std::make_move_iterator(loaded.begin()), std::make_move_iterator(loaded.end()));
    while (items.size() > static_cast<size_t>(max_items))
    {
        items.erase(items.begin());
    }
    host::log(L"File Tray: restored %zu item(s).", items.size());
}

bool tray_module_enabled()
{
    std::lock_guard lock(g_settings_mutex);
    return g_settings.file_tray_module;
}

namespace
{

void add_paths_to_file_tray_core(const std::vector<std::wstring>& paths)
{
    int max_items = 10;
    {
        std::lock_guard lock(g_settings_mutex);
        max_items = std::max(1, g_settings.file_tray_max_items);
    }

    std::vector<FileTrayItem> added;
    added.reserve(paths.size());
    for (const std::wstring& path : paths)
    {
        FileTrayItem item;
        item.path = path;
        item.name = base_name_from_path(path);
        if (item.name.empty())
        {
            item.name = path;
        }

        WIN32_FILE_ATTRIBUTE_DATA attr = {};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr))
        {
            item.is_directory = (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            item.size_bytes = (static_cast<uint64_t>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
        }

        SHFILEINFOW info = {};
        if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) && info.hIcon)
        {
            item.icon = icon_to_pixels_ref(info.hIcon, 32);
            DestroyIcon(info.hIcon);
        }
        added.push_back(std::move(item));
    }

    if (added.empty())
    {
        return;
    }

    std::lock_guard lock(g_state_mutex);
    for (auto& item : added)
    {
        // Re-dropping a file promotes the existing entry instead of duplicating it.
        auto existing = std::find_if(g_state.file_tray_items.begin(), g_state.file_tray_items.end(),
                                     [&](const FileTrayItem& other)
                                     {
                                         return _wcsicmp(other.path.c_str(), item.path.c_str()) == 0;
                                     });
        if (existing != g_state.file_tray_items.end())
        {
            g_state.file_tray_items.erase(existing);
        }
        g_state.file_tray_items.push_back(std::move(item));
    }
    while (g_state.file_tray_items.size() > static_cast<size_t>(max_items))
    {
        g_state.file_tray_items.erase(g_state.file_tray_items.begin());
    }
}

}

// Adds files and persists the result. Callers use this; the Core version above
// only changes the in-memory list.
void add_paths_to_file_tray(const std::vector<std::wstring>& paths)
{
    add_paths_to_file_tray_core(paths);
    save_file_tray();
}

namespace
{

void remove_file_tray_item_by_path(const std::wstring& path)
{
    {
        std::lock_guard lock(g_state_mutex);
        auto& items = g_state.file_tray_items;
        items.erase(std::remove_if(items.begin(), items.end(),
                                   [&](const FileTrayItem& item)
                                   {
                                       return _wcsicmp(item.path.c_str(), path.c_str()) == 0;
                                   }),
                    items.end());
    }
    save_file_tray();
}

}

bool extract_drop_paths(IDataObject* data, std::vector<std::wstring>* out)
{
    if (!data || !out)
    {
        return false;
    }
    FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium = {};
    if (FAILED(data->GetData(&fmt, &medium)))
    {
        return false;
    }

    // An HDROP is just the HGLOBAL; DragQueryFile locks it itself.
    HDROP drop = reinterpret_cast<HDROP>(medium.hGlobal);
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i)
    {
        const UINT len = DragQueryFileW(drop, i, nullptr, 0);
        if (len == 0)
        {
            continue;
        }
        std::wstring path(len, L'\0');
        DragQueryFileW(drop, i, path.data(), len + 1);
        out->push_back(std::move(path));
    }
    ReleaseStgMedium(&medium);
    return !out->empty();
}

namespace
{

void add_text_to_file_tray(const std::wstring& raw_text)
{
    std::wstring text = raw_text;
    while (!text.empty() && iswspace(text.back()))
    {
        text.pop_back();
    }
    if (text.empty())
    {
        return;
    }
    constexpr size_t k_max_chars = 100000;
    if (text.size() > k_max_chars)
    {
        text.resize(k_max_chars);
    }

    // One-line preview for the row: whitespace collapsed, capped.
    constexpr size_t preview_chars = 60;
    std::wstring preview;
    bool last_space = true;
    bool truncated = false;
    for (wchar_t ch : text)
    {
        if (ch == L'\r' || ch == L'\n' || ch == L'\t')
        {
            ch = L' ';
        }
        if (ch == L' ')
        {
            if (last_space)
            {
                continue;
            }
            last_space = true;
        }
        else
        {
            last_space = false;
        }
        if (preview.size() >= preview_chars)
        {
            truncated = true;
            break;
        }
        preview.push_back(ch);
    }
    while (!preview.empty() && preview.back() == L' ')
    {
        preview.pop_back();
    }
    if (truncated)
    {
        preview += L"...";
    }

    int max_items = 10;
    {
        std::lock_guard lock(g_settings_mutex);
        max_items = std::max(1, g_settings.file_tray_max_items);
    }

    FileTrayItem item;
    item.is_text = true;
    item.text = std::move(text);
    item.name = preview.empty() ? std::wstring(L"Text") : preview;

    std::lock_guard lock(g_state_mutex);
    auto& items = g_state.file_tray_items;
    // Pasting the same snippet again promotes it instead of duplicating it.
    items.erase(std::remove_if(items.begin(), items.end(),
                               [&](const FileTrayItem& other)
                               {
                                   return other.is_text && other.text == item.text;
                               }),
                items.end());
    items.push_back(std::move(item));
    while (items.size() > static_cast<size_t>(max_items))
    {
        items.erase(items.begin());
    }
}

}

void copy_text_to_clipboard(HWND hwnd, const std::wstring& text)
{
    if (text.empty() || !OpenClipboard(hwnd))
    {
        return;
    }
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem)
    {
        void* dst = GlobalLock(mem);
        if (dst)
        {
            memcpy(dst, text.c_str(), bytes);
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem))
            {
                GlobalFree(mem);
            }
        }
        else
        {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

// Pastes whatever is on the clipboard into the tray: copied files first
// (as references), otherwise copied text.
void paste_into_file_tray(HWND hwnd)
{
    std::vector<std::wstring> paths;
    if (OpenClipboard(hwnd))
    {
        HANDLE data = GetClipboardData(CF_HDROP);
        if (data)
        {
            HDROP drop = reinterpret_cast<HDROP>(data);
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i)
            {
                const UINT len = DragQueryFileW(drop, i, nullptr, 0);
                if (len == 0)
                {
                    continue;
                }
                std::wstring path(len, L'\0');
                DragQueryFileW(drop, i, path.data(), len + 1);
                paths.push_back(std::move(path));
            }
        }
        CloseClipboard();
    }

    if (!paths.empty())
    {
        add_paths_to_file_tray(paths);
    }
    else
    {
        add_text_to_file_tray(read_clipboard_text(hwnd));
        save_file_tray();
    }

    const int tab = file_tray_tab_index(get_settings_copy());
    if (tab >= 0)
    {
        g_idle_tab = tab;
    }
    g_click_expanded = true;
    g_layout_dirty = true;
    trigger_nudge();
}

DWORD pick_tray_drop_effect(DWORD allowed)
{
    // The tray only references files, so never ask the source to move them.
    if (allowed & DROPEFFECT_COPY)
    {
        return DROPEFFECT_COPY;
    }
    if (allowed & DROPEFFECT_LINK)
    {
        return DROPEFFECT_LINK;
    }
    return DROPEFFECT_NONE;
}

namespace
{

class TrayDropSource final : public IDropSource
{
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
        {
            return E_POINTER;
        }
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IDropSource)))
        {
            *ppv = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return static_cast<ULONG>(++refs_);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        const LONG r = --refs_;
        if (r == 0)
        {
            delete this;
        }
        return static_cast<ULONG>(r);
    }

    STDMETHODIMP QueryContinueDrag(BOOL esc_pressed, DWORD key_state) override
    {
        if (esc_pressed)
        {
            return DRAGDROP_S_CANCEL;
        }
        if (!(key_state & MK_LBUTTON))
        {
            return DRAGDROP_S_DROP;
        }
        return S_OK;
    }

    STDMETHODIMP GiveFeedback(DWORD) override
    {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    std::atomic<LONG> refs_{1};
};

// Builds the same data object Explorer would hand out for this file, so any
// app, folder or the desktop accepts it as a normal file drag.
bool create_file_data_object(const std::wstring& path, IDataObject** out)
{
    *out = nullptr;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl)
    {
        return false;
    }

    ComPtr<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;
    HRESULT hr = SHBindToParent(pidl, IID_PPV_ARGS(&parent), &child);
    if (SUCCEEDED(hr))
    {
        hr = parent->GetUIObjectOf(nullptr, 1, &child, __uuidof(IDataObject), nullptr, reinterpret_cast<void**>(out));
    }
    CoTaskMemFree(pidl);
    return SUCCEEDED(hr) && *out != nullptr;
}

}

// Blocks (modal OLE loop) until the drag finishes.
void start_file_tray_drag(const std::wstring& path)
{
    ComPtr<IDataObject> data_object;
    if (!create_file_data_object(path, data_object.GetAddressOf()))
    {
        return;
    }

    ComPtr<IDropSource> source;
    source.Attach(new TrayDropSource());

    constexpr DWORD ok_effects = DROPEFFECT_COPY | DROPEFFECT_LINK;
    DWORD effect = DROPEFFECT_NONE;

    // SHDoDragDrop adds the normal Explorer drag image (thumbnail + name).
    // Resolved dynamically; plain DoDragDrop is the fallback.
    using ShDoDragDropT = HRESULT(WINAPI*)(HWND, IDataObject*, IDropSource*, DWORD, DWORD*);
    static const auto p_sh_do_drag_drop =
        reinterpret_cast<ShDoDragDropT>(GetProcAddress(GetModuleHandleW(L"shell32.dll"), "SHDoDragDrop"));

    g_tray_internal_drag = true;
    if (p_sh_do_drag_drop)
    {
        p_sh_do_drag_drop(nullptr, data_object.Get(), source.Get(), ok_effects, &effect);
    }
    else
    {
        DoDragDrop(data_object.Get(), source.Get(), ok_effects, &effect);
    }
    g_tray_internal_drag = false;

    g_hovered_file_tray_row = -1;
    g_hovered_file_tray_action = -1;
    g_layout_dirty = true;

    // File was moved away by the target: its tray entry is now stale.
    if (effect != DROPEFFECT_NONE && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        remove_file_tray_item_by_path(path);
    }
}

}
