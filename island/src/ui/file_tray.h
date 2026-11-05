#pragma once

#include "core/common.h"
#include "core/state.h"
#include "core/tabs.h"

namespace island
{
namespace file_tray_store
{
inline std::mutex g_io_mutex;
bool resolve_paths(std::wstring* dir, std::wstring* file);
std::string escape(const std::wstring& value);
std::wstring unescape(const std::string& value);

}
}

namespace island
{
void save_file_tray();
void load_file_tray();
bool tray_module_enabled();
void add_paths_to_file_tray(const std::vector<std::wstring>& paths);
bool extract_drop_paths(IDataObject* data, std::vector<std::wstring>* out);
void copy_text_to_clipboard(HWND hwnd, const std::wstring& text);
void paste_into_file_tray(HWND hwnd);
DWORD pick_tray_drop_effect(DWORD allowed);

// Receives drags while they hover, so the island can expand onto the File Tray before the drop.
class TrayDropTarget final : public IDropTarget
{
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
        {
            return E_POINTER;
        }
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IDropTarget)))
        {
            *ppv = static_cast<IDropTarget*>(this);
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

    STDMETHODIMP DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override
    {
        accept_ = false;
        if (!g_tray_internal_drag.load() && tray_module_enabled() && data)
        {
            FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            accept_ = (data->QueryGetData(&fmt) == S_OK);
        }

        if (accept_)
        {
            tab_ = file_tray_tab_index(get_settings_copy());
            if (tab_ >= 0)
            {
                g_idle_tab = tab_;
            }
            g_tray_drag_over = true;
            g_click_expanded = true;
            g_layout_dirty = true;
            trigger_nudge();
        }
        if (effect)
        {
            *effect = accept_ ? pick_tray_drop_effect(*effect) : DROPEFFECT_NONE;
        }
        return S_OK;
    }

    STDMETHODIMP DragOver(DWORD, POINTL, DWORD* effect) override
    {
        if (accept_ && tab_ >= 0)
        {
            g_idle_tab = tab_;
        }
        if (effect)
        {
            *effect = accept_ ? pick_tray_drop_effect(*effect) : DROPEFFECT_NONE;
        }
        return S_OK;
    }

    STDMETHODIMP DragLeave() override
    {
        accept_ = false;
        g_tray_drag_over = false;
        g_layout_dirty = true;
        return S_OK;
    }

    STDMETHODIMP Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override
    {
        std::vector<std::wstring> paths;
        const bool ok = accept_ && extract_drop_paths(data, &paths);
        accept_ = false;

        if (ok)
        {
            add_paths_to_file_tray(paths);
            const int tab = file_tray_tab_index(get_settings_copy());
            if (tab >= 0)
            {
                g_idle_tab = tab;
            }
            g_click_expanded = true;
            g_layout_dirty = true;
            trigger_nudge();
            if (effect)
            {
                *effect = pick_tray_drop_effect(*effect);
            }
        }
        else if (effect)
        {
            *effect = DROPEFFECT_NONE;
        }
        g_tray_drag_over = false;
        return S_OK;
    }

private:
    std::atomic<LONG> refs_{1};
    bool accept_ = false;
    int tab_ = -1;
};

void start_file_tray_drag(const std::wstring& path);

}
