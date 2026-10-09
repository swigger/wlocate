#include "shell.hpp"
#include <shellapi.h>
#include <shlwapi.h>

namespace wl {
namespace {
ComPtr<IContextMenu2> active2;
ComPtr<IContextMenu3> active3;
void check(HRESULT result, const char* action) { if (FAILED(result)) throw Error(std::string(action) + ": " + win_error(static_cast<DWORD>(result))); }
struct IdLists {
    std::vector<PIDLIST_ABSOLUTE> values;
    explicit IdLists(const std::vector<std::wstring>& paths) {
        for (auto& path : paths) {
            PIDLIST_ABSOLUTE id = nullptr; auto hr = SHParseDisplayName(path.c_str(), nullptr, &id, 0, nullptr);
            if (FAILED(hr)) { for (auto item : values) CoTaskMemFree(item); throw Error("File is no longer available: " + utf8(path)); }
            values.push_back(id);
        }
    }
    ~IdLists() { for (auto id : values) CoTaskMemFree(id); }
};
ComPtr<IShellItemArray> items(const IdLists& ids) {
    ComPtr<IShellItemArray> array;
    check(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.values.size()), const_cast<PCIDLIST_ABSOLUTE*>(ids.values.data()), &array), "Create shell selection");
    return array;
}
ComPtr<IContextMenu> context(HWND owner, const std::vector<std::wstring>& paths) {
    if (paths.empty()) throw Error("No files selected");
    IdLists ids(paths); auto array = items(ids); ComPtr<IContextMenu> menu;
    auto hr = array->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&menu));
    if (FAILED(hr)) {
        ComPtr<IShellFolder> desktop; check(SHGetDesktopFolder(&desktop), "Get desktop folder");
        DEFCONTEXTMENU definition{}; definition.hwnd = owner; definition.psf = desktop.Get();
        std::vector<PCUITEMID_CHILD> children(ids.values.begin(), ids.values.end());
        definition.cidl = static_cast<UINT>(children.size()); definition.apidl = children.data();
        check(SHCreateDefaultContextMenu(&definition, IID_PPV_ARGS(&menu)), "Create shell context menu");
    }
    return menu;
}
void invoke(HWND owner, IContextMenu* menu, const char* verb, bool shift) {
    CMINVOKECOMMANDINFOEX info{}; info.cbSize = sizeof(info); info.hwnd = owner;
    info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_ASYNCOK | (shift ? CMIC_MASK_SHIFT_DOWN : 0);
    info.lpVerb = verb; std::wstring unicode;
    if (HIWORD(verb)) { unicode = wide(verb); info.lpVerbW = unicode.c_str(); } else info.lpVerbW = reinterpret_cast<LPCWSTR>(verb);
    info.nShow = SW_SHOWNORMAL;
    check(menu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info)), "Shell action");
}
class DropSource final : public IDropSource {
    LONG references_ = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (iid == IID_IUnknown || iid == IID_IDropSource) { *value = static_cast<IDropSource*>(this); AddRef(); return S_OK; } *value = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override { auto n = InterlockedDecrement(&references_); if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override { return escape ? DRAGDROP_S_CANCEL : !(keys & MK_LBUTTON) ? DRAGDROP_S_DROP : S_OK; }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};
}
LRESULT shell_menu_message(UINT message, WPARAM wparam, LPARAM lparam, bool& handled) {
    handled = false;
    if (message != WM_INITMENUPOPUP && message != WM_DRAWITEM && message != WM_MEASUREITEM && message != WM_MENUCHAR) return 0;
    LRESULT result = 0;
    if (active3 && SUCCEEDED(active3->HandleMenuMsg2(message, wparam, lparam, &result))) handled = true;
    else if (active2 && SUCCEEDED(active2->HandleMenuMsg(message, wparam, lparam))) handled = true;
    return result;
}
UINT shell_context(HWND owner, const std::vector<std::wstring>& paths, POINT point) {
    auto context_menu = context(owner, paths); HMENU popup = CreatePopupMenu();
    bool shift = GetKeyState(VK_SHIFT) < 0;
    auto hr = context_menu->QueryContextMenu(popup, 0, 1, 0x6FFF, CMF_NORMAL | CMF_CANRENAME | (shift ? CMF_EXTENDEDVERBS : 0));
    if (FAILED(hr)) { DestroyMenu(popup); check(hr, "Build shell menu"); }
    AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(popup, MF_STRING, shell_open_parent, L"打开所在文件夹(&F)\tCtrl+Enter");
    AppendMenuW(popup, MF_STRING, shell_copy_path, L"复制完整路径(&P)\tCtrl+Shift+C");
    context_menu.As(&active2); context_menu.As(&active3);
    UINT command = TrackPopupMenuEx(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, owner, nullptr);
    active3.Reset(); active2.Reset(); DestroyMenu(popup);
    if (command > 0 && command < 0x7000) {
        wchar_t verb[128]{};
        context_menu->GetCommandString(command - 1, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(verb), 128);
        if (!_wcsicmp(verb, L"rename")) return shell_rename_selected;
        invoke(owner, context_menu.Get(), MAKEINTRESOURCEA(command - 1), shift);
    }
    return command;
}
void shell_verb(HWND owner, const std::vector<std::wstring>& paths, const char* verb, bool shift) {
    auto menu = context(owner, paths); auto popup = CreatePopupMenu();
    auto hr = menu->QueryContextMenu(popup, 0, 1, 0x6FFF, CMF_NORMAL | CMF_CANRENAME); DestroyMenu(popup);
    check(hr, "Build shell actions"); invoke(owner, menu.Get(), verb, shift);
}
void shell_open(HWND owner, const std::wstring& path, bool parent) {
    if (parent) {
        IdLists ids({path}); check(SHOpenFolderAndSelectItems(ids.values.front(), 0, nullptr, 0), "Open containing folder");
    } else {
        SHELLEXECUTEINFOW info{sizeof(info)}; info.hwnd = owner; info.lpFile = path.c_str(); info.nShow = SW_SHOWNORMAL; info.fMask = SEE_MASK_FLAG_NO_UI;
        if (!ShellExecuteExW(&info)) throw Error("Open file: " + win_error());
    }
}
void clipboard_paths(HWND owner, const std::vector<std::wstring>& paths, bool cut, bool text_only) {
    if (paths.empty()) return;
    if (!text_only) { shell_verb(owner, paths, cut ? "cut" : "copy"); return; }
    std::wstring text;
    for (auto& path : paths) { if (!text.empty()) text += L"\r\n"; text += path; }
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t)); if (!memory) throw Error("Cannot allocate clipboard data");
    auto* data = GlobalLock(memory); memcpy(data, text.c_str(), (text.size() + 1) * sizeof(wchar_t)); GlobalUnlock(memory);
    if (!OpenClipboard(owner)) { GlobalFree(memory); throw Error("Clipboard is busy"); }
    EmptyClipboard(); if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory); CloseClipboard();
}
void shell_drag(HWND, const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    IdLists ids(paths); auto array = items(ids); ComPtr<IDataObject> data;
    check(array->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data)), "Create drag data");
    ComPtr<IDropSource> source; source.Attach(new DropSource()); DWORD effect;
    DoDragDrop(data.Get(), source.Get(), DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
}
void shell_rename(HWND owner, const std::wstring& path, const std::wstring& name) {
    if (name.empty() || name.find_first_of(L"\\/:*?\"<>|") != name.npos || name == L"." || name == L"..") throw Error("Invalid file name");
    ComPtr<IFileOperation> operation; check(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&operation)), "Create rename operation");
    operation->SetOwnerWindow(owner); operation->SetOperationFlags(FOF_ALLOWUNDO | FOFX_ADDUNDORECORD);
    ComPtr<IShellItem> item; check(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)), "Open rename item");
    check(operation->RenameItem(item.Get(), name.c_str(), nullptr), "Rename file"); check(operation->PerformOperations(), "Rename file");
}
std::vector<std::string> shell_menu_verbs(HWND owner, const std::vector<std::wstring>& paths) {
    auto menu = context(owner, paths); auto popup = CreatePopupMenu();
    auto hr = menu->QueryContextMenu(popup, 0, 1, 0x6FFF, CMF_NORMAL | CMF_CANRENAME);
    if (FAILED(hr)) { DestroyMenu(popup); check(hr, "Build shell menu"); }
    std::vector<std::string> verbs;
    for (UINT id = 0; id < static_cast<UINT>(HRESULT_CODE(hr)); ++id) {
        wchar_t verb[256]{};
        if (SUCCEEDED(menu->GetCommandString(id, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(verb), 256)) && *verb) verbs.push_back(utf8(verb));
    }
    DestroyMenu(popup); return verbs;
}
}
