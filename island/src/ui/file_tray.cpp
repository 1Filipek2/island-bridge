#include "ui/file_tray.h"
#include "notifications/capture.h"
#include "ui/icons.h"
#include "media/lyrics_cache.h"
#include "core/model.h"

namespace island
{
namespace FileTrayStore
{
namespace
{


inline std::wstring GetCurrentUserSid() {
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        DWORD len = 0;
        GetTokenInformation(hToken, TokenUser, nullptr, 0, &len);
        if (len > 0) {
            std::vector<BYTE> buffer(len);
            if (GetTokenInformation(hToken, TokenUser, buffer.data(), len, &len)) {
                auto* tokenUser = reinterpret_cast<TOKEN_USER*>(buffer.data());
                LPWSTR sidString = nullptr;
                if (ConvertSidToStringSidW(tokenUser->User.Sid, &sidString)) {
                    std::wstring result = sidString;
                    LocalFree(sidString);
                    CloseHandle(hToken);
                    return result;
                }
            }
        }
        CloseHandle(hToken);
    }
    wchar_t userBuf[256] = {};
    DWORD un = ARRAYSIZE(userBuf);
    if (GetUserNameW(userBuf, &un) && un > 0) {
        return userBuf;
    }
    return L"default";
}

}
}

namespace FileTrayStore
{


bool ResolvePaths(std::wstring* dir, std::wstring* file) {
    wchar_t storagePath[MAX_PATH] = {};
    if (!host::storage_path(storagePath, ARRAYSIZE(storagePath))) {
        return false;
    }
    const std::wstring userKey = GetCurrentUserSid();
    *dir = std::wstring(storagePath) + L"\\filetray\\" + userKey;
    *file = *dir + L"\\items.dat";
    return true;
}

std::string Escape(const std::wstring& value) {
    std::string out;
    for (const char c : LyricsCache::WideToUtf8(value)) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::wstring Unescape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            const char next = value[++i];
            switch (next) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: out.push_back(next); break;
            }
        } else {
            out.push_back(value[i]);
        }
    }
    return LyricsCache::Utf8ToWide(out);
}

}



// Writes the current tray to disk. Safe to call from any thread, and never call
// it while holding g_stateMutex. With persistence off, or an empty tray, the
// saved file is removed instead.
void SaveFileTray() {
    bool persist = true;
    {
        std::lock_guard lock(g_settingsMutex);
        persist = g_settings.fileTrayPersist;
    }

    struct Row {
        bool isText;
        std::wstring name;
        std::wstring value;
    };
    std::vector<Row> rows;
    {
        std::lock_guard lock(g_stateMutex);
        rows.reserve(g_state.fileTrayItems.size());
        for (const FileTrayItem& item : g_state.fileTrayItems) {
            rows.push_back(Row{item.isText, item.name, item.isText ? item.text : item.path});
        }
    }

    std::lock_guard io(FileTrayStore::g_ioMutex);
    std::wstring dir, file;
    if (!FileTrayStore::ResolvePaths(&dir, &file)) {
        return;
    }

    if (!persist || rows.empty()) {
        DeleteFileW(file.c_str());
        return;
    }

    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);

    std::string data = "DIWFT 1\n";
    for (const Row& row : rows) {
        if (row.isText) {
            data += "T\t" + FileTrayStore::Escape(row.name) + "\t" +
                    FileTrayStore::Escape(row.value) + "\n";
        } else {
            data += "F\t" + FileTrayStore::Escape(row.value) + "\n";
        }
    }

    if (!LyricsCache::WriteFileAtomic(file, data)) {
        host::log(L"File Tray: could not save %s (error %lu).", file.c_str(), GetLastError());
    }
}

// Reads the saved tray back. Runs once on the render thread at startup (it needs
// COM for the shell icons). Files that have since moved or been deleted are kept,
// since you asked for items to stay until you remove them; they just show a
// generic icon.
void LoadFileTray() {
    bool persist = true;
    int maxItems = 10;
    {
        std::lock_guard lock(g_settingsMutex);
        persist = g_settings.fileTrayPersist;
        maxItems = std::max(1, g_settings.fileTrayMaxItems);
    }

    std::lock_guard io(FileTrayStore::g_ioMutex);
    std::wstring dir, file;
    if (!FileTrayStore::ResolvePaths(&dir, &file)) {
        return;
    }
    if (!persist) {
        DeleteFileW(file.c_str());
        return;
    }

    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }

    constexpr LONGLONG kMaxBytes = 16ll * 1024 * 1024;
    std::string data;
    LARGE_INTEGER size = {};
    bool readOk = GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart <= kMaxBytes;
    if (readOk) {
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        readOk = ReadFile(h, data.data(), static_cast<DWORD>(size.QuadPart), &got, nullptr) &&
                 got == static_cast<DWORD>(size.QuadPart);
    }
    CloseHandle(h);

    static const char kHeader[] = "DIWFT 1\n";
    if (!readOk || data.compare(0, sizeof(kHeader) - 1, kHeader) != 0) {
        host::log(L"File Tray: saved list is unreadable; starting empty.");
        return;
    }

    std::vector<FileTrayItem> loaded;
    size_t pos = sizeof(kHeader) - 1;
    while (pos < data.size()) {
        size_t end = data.find('\n', pos);
        if (end == std::string::npos) end = data.size();
        const std::string line = data.substr(pos, end - pos);
        pos = end + 1;

        if (line.size() < 3 || line[1] != '\t') {
            continue;
        }

        FileTrayItem item;
        if (line[0] == 'T') {
            const std::string rest = line.substr(2);
            const size_t tab = rest.find('\t');
            if (tab == std::string::npos) continue;
            item.isText = true;
            item.name = FileTrayStore::Unescape(rest.substr(0, tab));
            item.text = FileTrayStore::Unescape(rest.substr(tab + 1));
            if (item.text.empty()) continue;
            if (item.name.empty()) item.name = L"Text";
        } else if (line[0] == 'F') {
            item.path = FileTrayStore::Unescape(line.substr(2));
            if (item.path.empty()) continue;
            item.name = BaseNameFromPath(item.path);
            if (item.name.empty()) item.name = item.path;

            WIN32_FILE_ATTRIBUTE_DATA attr = {};
            const bool exists =
                GetFileAttributesExW(item.path.c_str(), GetFileExInfoStandard, &attr) != 0;
            if (exists) {
                item.isDirectory = (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                item.sizeBytes =
                    (static_cast<uint64_t>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
            }

            // A missing file gets its type icon from the extension alone, which
            // never touches the disk (so an unplugged drive can't stall startup).
            SHFILEINFOW info = {};
            UINT flags = SHGFI_ICON | SHGFI_LARGEICON;
            if (!exists) flags |= SHGFI_USEFILEATTRIBUTES;
            if (SHGetFileInfoW(item.path.c_str(), exists ? 0 : FILE_ATTRIBUTE_NORMAL, &info,
                               sizeof(info), flags) &&
                info.hIcon) {
                item.icon = IconToPixelsRef(info.hIcon, 32);
                DestroyIcon(info.hIcon);
            }
        } else {
            continue;
        }
        loaded.push_back(std::move(item));
    }

    if (loaded.empty()) {
        return;
    }

    std::lock_guard lock(g_stateMutex);
    auto& items = g_state.fileTrayItems;
    // Saved items are older than anything dropped since startup, so they go in front.
    items.insert(items.begin(), std::make_move_iterator(loaded.begin()),
                 std::make_move_iterator(loaded.end()));
    while (items.size() > static_cast<size_t>(maxItems)) {
        items.erase(items.begin());
    }
    host::log(L"File Tray: restored %zu item(s).", items.size());
}

bool TrayModuleEnabled() {
    std::lock_guard lock(g_settingsMutex);
    return g_settings.fileTrayModule;
}


namespace
{


void AddPathsToFileTrayCore(const std::vector<std::wstring>& paths) {
    int maxItems = 10;
    {
        std::lock_guard lock(g_settingsMutex);
        maxItems = std::max(1, g_settings.fileTrayMaxItems);
    }

    std::vector<FileTrayItem> added;
    added.reserve(paths.size());
    for (const std::wstring& path : paths) {
        FileTrayItem item;
        item.path = path;
        item.name = BaseNameFromPath(path);
        if (item.name.empty()) {
            item.name = path;
        }

        WIN32_FILE_ATTRIBUTE_DATA attr = {};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr)) {
            item.isDirectory = (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            item.sizeBytes = (static_cast<uint64_t>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
        }

        SHFILEINFOW info = {};
        if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) && info.hIcon) {
            item.icon = IconToPixelsRef(info.hIcon, 32);
            DestroyIcon(info.hIcon);
        }
        added.push_back(std::move(item));
    }

    if (added.empty()) {
        return;
    }

    std::lock_guard lock(g_stateMutex);
    for (auto& item : added) {
        // Re-dropping a file promotes the existing entry instead of duplicating it.
        auto existing = std::find_if(g_state.fileTrayItems.begin(), g_state.fileTrayItems.end(),
                                     [&](const FileTrayItem& other) {
                                         return _wcsicmp(other.path.c_str(), item.path.c_str()) == 0;
                                     });
        if (existing != g_state.fileTrayItems.end()) {
            g_state.fileTrayItems.erase(existing);
        }
        g_state.fileTrayItems.push_back(std::move(item));
    }
    while (g_state.fileTrayItems.size() > static_cast<size_t>(maxItems)) {
        g_state.fileTrayItems.erase(g_state.fileTrayItems.begin());
    }
}

}



// Adds files and persists the result. Callers use this; the Core version above
// only changes the in-memory list.
void AddPathsToFileTray(const std::vector<std::wstring>& paths) {
    AddPathsToFileTrayCore(paths);
    SaveFileTray();
}


namespace
{


void RemoveFileTrayItemByPath(const std::wstring& path) {
    {
        std::lock_guard lock(g_stateMutex);
        auto& items = g_state.fileTrayItems;
        items.erase(std::remove_if(items.begin(), items.end(),
                                   [&](const FileTrayItem& item) {
                                       return _wcsicmp(item.path.c_str(), path.c_str()) == 0;
                                   }),
                    items.end());
    }
    SaveFileTray();
}

}



bool ExtractDropPaths(IDataObject* data, std::vector<std::wstring>* out) {
    if (!data || !out) {
        return false;
    }
    FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium = {};
    if (FAILED(data->GetData(&fmt, &medium))) {
        return false;
    }

    // An HDROP is just the HGLOBAL; DragQueryFile locks it itself.
    HDROP drop = reinterpret_cast<HDROP>(medium.hGlobal);
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        const UINT len = DragQueryFileW(drop, i, nullptr, 0);
        if (len == 0) {
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


void AddTextToFileTray(const std::wstring& rawText) {
    std::wstring text = rawText;
    while (!text.empty() && iswspace(text.back())) {
        text.pop_back();
    }
    if (text.empty()) {
        return;
    }
    constexpr size_t kMaxChars = 100000;
    if (text.size() > kMaxChars) {
        text.resize(kMaxChars);
    }

    // One-line preview for the row: whitespace collapsed, capped.
    constexpr size_t kPreviewChars = 60;
    std::wstring preview;
    bool lastSpace = true;
    bool truncated = false;
    for (wchar_t ch : text) {
        if (ch == L'\r' || ch == L'\n' || ch == L'\t') {
            ch = L' ';
        }
        if (ch == L' ') {
            if (lastSpace) continue;
            lastSpace = true;
        } else {
            lastSpace = false;
        }
        if (preview.size() >= kPreviewChars) {
            truncated = true;
            break;
        }
        preview.push_back(ch);
    }
    while (!preview.empty() && preview.back() == L' ') {
        preview.pop_back();
    }
    if (truncated) {
        preview += L"...";
    }

    int maxItems = 10;
    {
        std::lock_guard lock(g_settingsMutex);
        maxItems = std::max(1, g_settings.fileTrayMaxItems);
    }

    FileTrayItem item;
    item.isText = true;
    item.text = std::move(text);
    item.name = preview.empty() ? std::wstring(L"Text") : preview;

    std::lock_guard lock(g_stateMutex);
    auto& items = g_state.fileTrayItems;
    // Pasting the same snippet again promotes it instead of duplicating it.
    items.erase(std::remove_if(items.begin(), items.end(),
                               [&](const FileTrayItem& other) {
                                   return other.isText && other.text == item.text;
                               }),
                items.end());
    items.push_back(std::move(item));
    while (items.size() > static_cast<size_t>(maxItems)) {
        items.erase(items.begin());
    }
}

}



void CopyTextToClipboard(HWND hwnd, const std::wstring& text) {
    if (text.empty() || !OpenClipboard(hwnd)) {
        return;
    }
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        void* dst = GlobalLock(mem);
        if (dst) {
            memcpy(dst, text.c_str(), bytes);
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) {
                GlobalFree(mem);
            }
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

// Pastes whatever is on the clipboard into the tray: copied files first
// (as references), otherwise copied text.
void PasteIntoFileTray(HWND hwnd) {
    std::vector<std::wstring> paths;
    if (OpenClipboard(hwnd)) {
        HANDLE data = GetClipboardData(CF_HDROP);
        if (data) {
            HDROP drop = reinterpret_cast<HDROP>(data);
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                const UINT len = DragQueryFileW(drop, i, nullptr, 0);
                if (len == 0) {
                    continue;
                }
                std::wstring path(len, L'\0');
                DragQueryFileW(drop, i, path.data(), len + 1);
                paths.push_back(std::move(path));
            }
        }
        CloseClipboard();
    }

    if (!paths.empty()) {
        AddPathsToFileTray(paths);
    } else {
        AddTextToFileTray(ReadClipboardText(hwnd));
        SaveFileTray();
    }

    const int tab = FileTrayTabIndex(GetSettingsCopy());
    if (tab >= 0) {
        g_idleTab = tab;
    }
    g_clickExpanded = true;
    g_layoutDirty = true;
    TriggerNudge();
}

DWORD PickTrayDropEffect(DWORD allowed) {
    // The tray only references files, so never ask the source to move them.
    if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
    if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
    return DROPEFFECT_NONE;
}


namespace
{


class TrayDropSource final : public IDropSource {
   public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IDropSource))) {
            *ppv = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG r = --refs_;
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }
    STDMETHODIMP QueryContinueDrag(BOOL escPressed, DWORD keyState) override {
        if (escPressed) return DRAGDROP_S_CANCEL;
        if (!(keyState & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }
    STDMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

   private:
    std::atomic<LONG> refs_{1};
};

// Builds the same data object Explorer would hand out for this file, so any
// app, folder or the desktop accepts it as a normal file drag.
bool CreateFileDataObject(const std::wstring& path, IDataObject** out) {
    *out = nullptr;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl) {
        return false;
    }

    ComPtr<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;
    HRESULT hr = SHBindToParent(pidl, IID_PPV_ARGS(&parent), &child);
    if (SUCCEEDED(hr)) {
        hr = parent->GetUIObjectOf(nullptr, 1, &child, __uuidof(IDataObject), nullptr,
                                   reinterpret_cast<void**>(out));
    }
    CoTaskMemFree(pidl);
    return SUCCEEDED(hr) && *out != nullptr;
}

}



// Blocks (modal OLE loop) until the drag finishes.
void StartFileTrayDrag(const std::wstring& path) {
    ComPtr<IDataObject> dataObject;
    if (!CreateFileDataObject(path, dataObject.GetAddressOf())) {
        return;
    }

    ComPtr<IDropSource> source;
    source.Attach(new TrayDropSource());

    constexpr DWORD kOkEffects = DROPEFFECT_COPY | DROPEFFECT_LINK;
    DWORD effect = DROPEFFECT_NONE;

    // SHDoDragDrop adds the normal Explorer drag image (thumbnail + name).
    // Resolved dynamically; plain DoDragDrop is the fallback.
    using SHDoDragDrop_t = HRESULT(WINAPI*)(HWND, IDataObject*, IDropSource*, DWORD, DWORD*);
    static const auto pSHDoDragDrop = reinterpret_cast<SHDoDragDrop_t>(
        GetProcAddress(GetModuleHandleW(L"shell32.dll"), "SHDoDragDrop"));

    g_trayInternalDrag = true;
    if (pSHDoDragDrop) {
        pSHDoDragDrop(nullptr, dataObject.Get(), source.Get(), kOkEffects, &effect);
    } else {
        DoDragDrop(dataObject.Get(), source.Get(), kOkEffects, &effect);
    }
    g_trayInternalDrag = false;

    g_hoveredFileTrayRow = -1;
    g_hoveredFileTrayAction = -1;
    g_layoutDirty = true;

    // File was moved away by the target: its tray entry is now stale.
    if (effect != DROPEFFECT_NONE && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        RemoveFileTrayItemByPath(path);
    }
}


}
