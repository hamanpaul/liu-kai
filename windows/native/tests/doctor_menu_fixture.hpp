#pragma once
#include <algorithm>
#include <string>
#include <vector>

class DoctorMenuCapture final : public ITfMenu {
public:
    struct Entry { UINT id; DWORD flags; std::wstring text; };
    std::vector<Entry> entries;
    ULONG refs = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!IsEqualIID(iid, IID_IUnknown)) return E_NOINTERFACE;
        *out = static_cast<IUnknown*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE AddMenuItem(UINT id, DWORD flags, HBITMAP, HBITMAP,
            const WCHAR* text, ULONG count, ITfMenu**) override {
        entries.push_back({id, flags, std::wstring(text, count)});
        return S_OK;
    }
    bool contains(const std::wstring& text) const {
        return std::any_of(entries.begin(), entries.end(), [&](const Entry& e) {
            return e.text.find(text) != std::wstring::npos;
        });
    }
};
