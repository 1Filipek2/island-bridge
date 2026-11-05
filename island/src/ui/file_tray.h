#pragma once

#include "core/common.h"
#include "core/state.h"
#include "core/tabs.h"
namespace island
{
namespace FileTrayStore
{


inline std::mutex g_ioMutex;  // serialises the load/save file access
bool ResolvePaths(std::wstring* dir, std::wstring* file);
std::string Escape(const std::wstring& value);
std::wstring Unescape(const std::string& value);

}
}

namespace island
{

void SaveFileTray();
void LoadFileTray();
bool TrayModuleEnabled();
void AddPathsToFileTray(const std::vector<std::wstring>& paths);
bool ExtractDropPaths(IDataObject* data, std::vector<std::wstring>* out);
void CopyTextToClipboard(HWND hwnd, const std::wstring& text);
void PasteIntoFileTray(HWND hwnd);
DWORD PickTrayDropEffect(DWORD allowed);

// Receives drags while they are still hovering, which is what lets the island
// expand onto the File Tray *before* the drop.
class TrayDropTarget final : public IDropTarget {
   public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IDropTarget))) {
            *ppv = static_cast<IDropTarget*>(this);
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

    STDMETHODIMP DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        accept_ = false;
        if (!g_trayInternalDrag.load() && TrayModuleEnabled() && data) {
            FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            accept_ = (data->QueryGetData(&fmt) == S_OK);
        }

        if (accept_) {
            tab_ = FileTrayTabIndex(GetSettingsCopy());
            if (tab_ >= 0) {
                g_idleTab = tab_;
            }
            g_trayDragOver = true;
            g_clickExpanded = true;
            g_layoutDirty = true;
            TriggerNudge();
        }
        if (effect) {
            *effect = accept_ ? PickTrayDropEffect(*effect) : DROPEFFECT_NONE;
        }
        return S_OK;
    }

    STDMETHODIMP DragOver(DWORD, POINTL, DWORD* effect) override {
        if (accept_ && tab_ >= 0) {
            g_idleTab = tab_;  // stay on the tray while the island expands
        }
        if (effect) {
            *effect = accept_ ? PickTrayDropEffect(*effect) : DROPEFFECT_NONE;
        }
        return S_OK;
    }

    STDMETHODIMP DragLeave() override {
        accept_ = false;
        g_trayDragOver = false;
        g_layoutDirty = true;
        return S_OK;
    }

    STDMETHODIMP Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        std::vector<std::wstring> paths;
        const bool ok = accept_ && ExtractDropPaths(data, &paths);
        accept_ = false;

        if (ok) {
            AddPathsToFileTray(paths);
            const int tab = FileTrayTabIndex(GetSettingsCopy());
            if (tab >= 0) {
                g_idleTab = tab;
            }
            g_clickExpanded = true;
            g_layoutDirty = true;
            TriggerNudge();
            if (effect) *effect = PickTrayDropEffect(*effect);
        } else if (effect) {
            *effect = DROPEFFECT_NONE;
        }
        g_trayDragOver = false;
        return S_OK;
    }

   private:
    std::atomic<LONG> refs_{1};
    bool accept_ = false;
    int tab_ = -1;
};
void StartFileTrayDrag(const std::wstring& path);

}
