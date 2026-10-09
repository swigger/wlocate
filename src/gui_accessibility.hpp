#pragma once
#include "common.hpp"
#include <commctrl.h>
#include <UIAutomation.h>
#include <wrl/client.h>

namespace wl {
// The stock ListView UIA proxy enumerates every owner-data row, including all
// its cells, on the GUI thread. Expose a virtualized tree instead: visible rows
// are children; ItemContainer/VirtualizedItem provide access to other rows.
class AccessibleList {
    struct State {
        HWND list = nullptr;
        uint32_t epoch = 1;
        std::function<std::wstring(int)> name;
        std::function<void(int)> invoke;
        IRawElementProviderSimple* root = nullptr;
    };
    class Provider final : public IRawElementProviderSimple, public IRawElementProviderFragment,
        public IRawElementProviderFragmentRoot, public IItemContainerProvider, public IVirtualizedItemProvider,
        public ISelectionProvider, public ISelectionItemProvider, public IScrollItemProvider, public IInvokeProvider {
        std::atomic<ULONG> references_ = 1;
        std::shared_ptr<State> state_;
        int index_;
        uint32_t epoch_;
        bool valid() const { return state_->list && (index_ < 0 || (epoch_ == state_->epoch && index_ < ListView_GetItemCount(state_->list))); }
        int first() const { return std::max(0, ListView_GetTopIndex(state_->list)); }
        int last() const { return static_cast<int>(std::min<int64_t>(ListView_GetItemCount(state_->list) - 1, int64_t(first()) + ListView_GetCountPerPage(state_->list))); }
        bool visible() const { return index_ < 0 || (index_ >= first() && index_ <= last()); }
        Provider* child(int index) { return new Provider(state_, index); }
        HRESULT root(REFIID iid, void** output) { return valid() ? state_->root->QueryInterface(iid, output) : UIA_E_ELEMENTNOTAVAILABLE; }
    public:
        Provider(std::shared_ptr<State> state, int index) : state_(std::move(state)), index_(index), epoch_(state_->epoch) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
            if (!output) return E_POINTER; *output = nullptr;
            if (iid == __uuidof(IUnknown) || iid == __uuidof(IRawElementProviderSimple)) *output = static_cast<IRawElementProviderSimple*>(this);
            else if (iid == __uuidof(IRawElementProviderFragment)) *output = static_cast<IRawElementProviderFragment*>(this);
            else if (index_ < 0 && iid == __uuidof(IRawElementProviderFragmentRoot)) *output = static_cast<IRawElementProviderFragmentRoot*>(this);
            else if (index_ < 0 && iid == __uuidof(IItemContainerProvider)) *output = static_cast<IItemContainerProvider*>(this);
            else if (index_ < 0 && iid == __uuidof(ISelectionProvider)) *output = static_cast<ISelectionProvider*>(this);
            else if (index_ >= 0 && iid == __uuidof(IVirtualizedItemProvider)) *output = static_cast<IVirtualizedItemProvider*>(this);
            else if (index_ >= 0 && iid == __uuidof(ISelectionItemProvider)) *output = static_cast<ISelectionItemProvider*>(this);
            else if (index_ >= 0 && iid == __uuidof(IScrollItemProvider)) *output = static_cast<IScrollItemProvider*>(this);
            else if (index_ >= 0 && iid == __uuidof(IInvokeProvider)) *output = static_cast<IInvokeProvider*>(this);
            if (!*output) return E_NOINTERFACE; AddRef(); return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
        ULONG STDMETHODCALLTYPE Release() override { auto count = --references_; if (!count) delete this; return count; }
        HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* result) override {
            // Controls and their cached rows belong to the GUI STA. Marshal
            // provider calls there instead of cross-thread SendMessage per cell.
            *result = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading); return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID id, IUnknown** result) override {
            *result = nullptr; if (!valid()) return UIA_E_ELEMENTNOTAVAILABLE;
            if (index_ < 0) {
                if (id == UIA_ItemContainerPatternId) *result = static_cast<IItemContainerProvider*>(this);
                if (id == UIA_SelectionPatternId) *result = static_cast<ISelectionProvider*>(this);
            } else {
                if (id == UIA_VirtualizedItemPatternId) *result = static_cast<IVirtualizedItemProvider*>(this);
                if (id == UIA_SelectionItemPatternId) *result = static_cast<ISelectionItemProvider*>(this);
                if (id == UIA_ScrollItemPatternId) *result = static_cast<IScrollItemProvider*>(this);
                if (id == UIA_InvokePatternId) *result = static_cast<IInvokeProvider*>(this);
            }
            if (*result) AddRef(); return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* value) override {
            VariantInit(value); if (!valid()) return UIA_E_ELEMENTNOTAVAILABLE;
            try {
                auto boolean = [&](bool v) { value->vt = VT_BOOL; value->boolVal = v ? VARIANT_TRUE : VARIANT_FALSE; };
                auto string = [&](const std::wstring& v) { value->vt = VT_BSTR; value->bstrVal = SysAllocString(v.c_str()); return value->bstrVal ? S_OK : E_OUTOFMEMORY; };
                switch (id) {
                case UIA_ControlTypePropertyId: value->vt = VT_I4; value->lVal = index_ < 0 ? UIA_ListControlTypeId : UIA_ListItemControlTypeId; break;
                case UIA_NamePropertyId:
                    if (index_ >= 0 && !visible()) return UIA_E_ELEMENTNOTAVAILABLE;
                    return string(index_ < 0 ? L"搜索结果" : state_->name(index_));
                case UIA_AutomationIdPropertyId: return string(index_ < 0 ? L"Results" : L"row-" + std::to_wstring(index_));
                case UIA_ItemStatusPropertyId: if (index_ < 0) return string(std::to_wstring(ListView_GetItemCount(state_->list)) + L" 个结果"); break;
                case UIA_IsControlElementPropertyId: case UIA_IsContentElementPropertyId: case UIA_IsKeyboardFocusablePropertyId: boolean(true); break;
                case UIA_IsEnabledPropertyId: boolean(IsWindowEnabled(state_->list)); break;
                case UIA_HasKeyboardFocusPropertyId: boolean(::GetFocus() == state_->list && (index_ < 0 || ListView_GetNextItem(state_->list, -1, LVNI_FOCUSED) == index_)); break;
                case UIA_IsOffscreenPropertyId: boolean(!IsWindowVisible(state_->list) || !visible()); break;
                case UIA_FrameworkIdPropertyId: return string(L"Win32");
                }
                return S_OK;
            } catch (...) { return E_FAIL; }
        }
        HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** result) override {
            *result = nullptr; if (!valid()) return UIA_E_ELEMENTNOTAVAILABLE;
            return index_ < 0 ? UiaHostProviderFromHwnd(state_->list, result) : S_OK;
        }
        HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** result) override {
            *result = nullptr; if (!valid()) return UIA_E_ELEMENTNOTAVAILABLE;
            if (index_ < 0) {
                if (ListView_GetItemCount(state_->list) && (direction == NavigateDirection_FirstChild || direction == NavigateDirection_LastChild))
                    *result = child(direction == NavigateDirection_FirstChild ? first() : last());
            } else {
                if (direction == NavigateDirection_Parent) return root(__uuidof(IRawElementProviderFragment), reinterpret_cast<void**>(result));
                if (visible() && direction == NavigateDirection_NextSibling && index_ < last()) *result = child(index_ + 1);
                if (visible() && direction == NavigateDirection_PreviousSibling && index_ > first()) *result = child(index_ - 1);
            }
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** result) override {
            *result = nullptr; if (!valid()) return UIA_E_ELEMENTNOTAVAILABLE; if (index_ < 0) return S_OK;
            int id[]{UiaAppendRuntimeId, static_cast<int>(epoch_), index_};
            *result = SafeArrayCreateVector(VT_I4, 0, 3); if (!*result) return E_OUTOFMEMORY;
            for (LONG i = 0; i < 3; ++i) SafeArrayPutElement(*result, &i, &id[i]); return S_OK;
        }
        HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* result) override {
            *result = {}; if (!valid()) return UIA_E_ELEMENTNOTAVAILABLE;
            RECT rect{};
            if (index_ < 0) GetWindowRect(state_->list, &rect);
            else if (visible()) {
                ListView_GetItemRect(state_->list, index_, &rect, LVIR_BOUNDS);
                RECT client; GetClientRect(state_->list, &client); IntersectRect(&rect, &rect, &client);
                MapWindowPoints(state_->list, nullptr, reinterpret_cast<POINT*>(&rect), 2);
            }
            *result = {double(rect.left), double(rect.top), double(rect.right - rect.left), double(rect.bottom - rect.top)}; return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** result) override { *result = nullptr; return S_OK; }
        HRESULT STDMETHODCALLTYPE SetFocus() override {
            if (!valid()) return UIA_E_ELEMENTNOTAVAILABLE; ::SetFocus(state_->list);
            if (index_ >= 0) { ListView_EnsureVisible(state_->list, index_, FALSE); ListView_SetItemState(state_->list, index_, LVIS_FOCUSED, LVIS_FOCUSED); } return S_OK;
        }
        HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** result) override { *result = nullptr; return root(__uuidof(IRawElementProviderFragmentRoot), reinterpret_cast<void**>(result)); }
        HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** result) override {
            *result = nullptr; if (!valid()) return UIA_E_ELEMENTNOTAVAILABLE;
            LVHITTESTINFO hit{}; hit.pt = {static_cast<LONG>(x), static_cast<LONG>(y)}; ScreenToClient(state_->list, &hit.pt);
            auto index = ListView_HitTest(state_->list, &hit);
            if (index >= 0) *result = child(index); else return root(__uuidof(IRawElementProviderFragment), reinterpret_cast<void**>(result)); return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** result) override {
            *result = nullptr; if (!valid()) return UIA_E_ELEMENTNOTAVAILABLE;
            if (::GetFocus() == state_->list) { auto index = ListView_GetNextItem(state_->list, -1, LVNI_FOCUSED); if (index >= 0) *result = child(index); } return S_OK;
        }
        HRESULT STDMETHODCALLTYPE FindItemByProperty(IRawElementProviderSimple* after, PROPERTYID property, VARIANT value, IRawElementProviderSimple** result) override {
            *result = nullptr; if (!valid()) return UIA_E_ELEMENTNOTAVAILABLE;
            int index = 0;
            if (after) {
                auto previous = dynamic_cast<Provider*>(after);
                if (!previous || previous->state_ != state_ || !previous->valid() || previous->index_ < 0) return E_INVALIDARG;
                index = previous->index_ + 1;
            }
            if (property == UIA_AutomationIdPropertyId) {
                if (value.vt != VT_BSTR || !value.bstrVal || wcsncmp(value.bstrVal, L"row-", 4)) return E_INVALIDARG;
                wchar_t* end = nullptr; auto parsed = wcstoll(value.bstrVal + 4, &end, 10);
                if (*end || end == value.bstrVal + 4 || parsed < index || parsed > INT_MAX) return S_OK; index = static_cast<int>(parsed);
            } else if (property == UIA_SelectionItemIsSelectedPropertyId && value.vt == VT_BOOL && value.boolVal == VARIANT_TRUE) {
                index = ListView_GetNextItem(state_->list, index - 1, LVNI_SELECTED);
            } else if (property != 0) return UIA_E_NOTSUPPORTED;
            if (index >= 0 && index < ListView_GetItemCount(state_->list)) *result = child(index); return S_OK;
        }
        HRESULT STDMETHODCALLTYPE Realize() override { return ScrollIntoView(); }
        HRESULT STDMETHODCALLTYPE ScrollIntoView() override { if (!valid()) return UIA_E_ELEMENTNOTAVAILABLE; if (index_ >= 0) ListView_EnsureVisible(state_->list, index_, FALSE); return S_OK; }
        HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY** result) override {
            *result = nullptr; if (!valid()) return UIA_E_ELEMENTNOTAVAILABLE;
            auto count = ListView_GetSelectedCount(state_->list);
            // A select-all over millions of rows is represented by the
            // ItemContainer's IsSelected query, without a giant COM array.
            if (count > 4096) return UIA_E_NOTSUPPORTED;
            *result = SafeArrayCreateVector(VT_UNKNOWN, 0, count); if (!*result) return E_OUTOFMEMORY;
            LONG at = 0;
            for (int i = -1; (i = ListView_GetNextItem(state_->list, i, LVNI_SELECTED)) >= 0;) {
                auto item = static_cast<IRawElementProviderSimple*>(child(i)); SafeArrayPutElement(*result, &at, item); item->Release(); ++at;
            }
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE get_CanSelectMultiple(BOOL* result) override { *result = TRUE; return S_OK; }
        HRESULT STDMETHODCALLTYPE get_IsSelectionRequired(BOOL* result) override { *result = FALSE; return S_OK; }
        HRESULT STDMETHODCALLTYPE Select() override {
            if (!valid()) return UIA_E_ELEMENTNOTAVAILABLE; ListView_SetItemState(state_->list, -1, 0, LVIS_SELECTED); return AddToSelection();
        }
        HRESULT STDMETHODCALLTYPE AddToSelection() override { if (!valid()) return UIA_E_ELEMENTNOTAVAILABLE; ListView_SetItemState(state_->list, index_, LVIS_SELECTED, LVIS_SELECTED); return S_OK; }
        HRESULT STDMETHODCALLTYPE RemoveFromSelection() override { if (!valid()) return UIA_E_ELEMENTNOTAVAILABLE; ListView_SetItemState(state_->list, index_, 0, LVIS_SELECTED); return S_OK; }
        HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* result) override { if (!valid()) return UIA_E_ELEMENTNOTAVAILABLE; *result = (ListView_GetItemState(state_->list, index_, LVIS_SELECTED) != 0); return S_OK; }
        HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** result) override { *result = nullptr; return root(__uuidof(IRawElementProviderSimple), reinterpret_cast<void**>(result)); }
        HRESULT STDMETHODCALLTYPE Invoke() override { auto hr = Select(); if (SUCCEEDED(hr)) state_->invoke(index_); return hr; }
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
    Microsoft::WRL::ComPtr<IRawElementProviderSimple> provider_;
    static LRESULT CALLBACK subclass(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data) {
        auto self = reinterpret_cast<AccessibleList*>(data);
        if (message == WM_GETOBJECT && static_cast<LONG>(lparam) == UiaRootObjectId)
            return UiaReturnRawElementProvider(hwnd, wparam, lparam, self->provider_.Get());
        if (message == WM_NCDESTROY) { RemoveWindowSubclass(hwnd, subclass, id); self->detach(); }
        return DefSubclassProc(hwnd, message, wparam, lparam);
    }
public:
    ~AccessibleList() { detach(); }
    void attach(HWND list, std::function<std::wstring(int)> name, std::function<void(int)> invoke) {
        state_->list = list; state_->name = std::move(name); state_->invoke = std::move(invoke);
        provider_.Attach(new Provider(state_, -1)); state_->root = provider_.Get();
        SetWindowSubclass(list, subclass, 1, reinterpret_cast<DWORD_PTR>(this));
    }
    void detach() {
        if (state_->list) RemoveWindowSubclass(state_->list, subclass, 1);
        state_->list = nullptr; state_->name = {}; state_->invoke = {}; state_->root = nullptr;
        if (provider_) { UiaDisconnectProvider(provider_.Get()); provider_.Reset(); }
    }
    void reset() { ++state_->epoch; }
    IRawElementProviderSimple* provider() const { return provider_.Get(); }
};
}
