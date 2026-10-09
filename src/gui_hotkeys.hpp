#pragma once
#include "common.hpp"
#include "resource.h"
#include <commctrl.h>

namespace wl {
struct HotkeyConfig {
    bool enabled = true;
    UINT modifiers = MOD_CONTROL | MOD_ALT, key = 'L';
    bool operator==(const HotkeyConfig&) const = default;
};
inline const std::vector<UINT>& hotkey_keys() {
    static const auto keys = [] {
        std::vector<UINT> result;
        for (UINT key = 'A'; key <= 'Z'; ++key) result.push_back(key);
        for (UINT key = '0'; key <= '9'; ++key) result.push_back(key);
        for (UINT key = VK_F1; key <= VK_F24; ++key) if (key != VK_F12) result.push_back(key); // Reserved for the debugger by Windows.
        for (UINT key : {VK_SPACE, VK_TAB, VK_RETURN, VK_ESCAPE, VK_INSERT, VK_DELETE, VK_HOME, VK_END,
            VK_PRIOR, VK_NEXT, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_PAUSE,
            VK_OEM_1, VK_OEM_PLUS, VK_OEM_COMMA, VK_OEM_MINUS, VK_OEM_PERIOD, VK_OEM_2, VK_OEM_3, VK_OEM_4, VK_OEM_5, VK_OEM_6, VK_OEM_7}) result.push_back(key);
        for (UINT key = VK_NUMPAD0; key <= VK_DIVIDE; ++key) if (key != VK_SEPARATOR) result.push_back(key);
        return result;
    }();
    return keys;
}
inline std::wstring hotkey_key_name(UINT key) {
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) return std::wstring(1, static_cast<wchar_t>(key));
    if (key >= VK_F1 && key <= VK_F24) return L"F" + std::to_wstring(key - VK_F1 + 1);
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) return L"数字键盘 " + std::to_wstring(key - VK_NUMPAD0);
    switch (key) {
    case VK_SPACE: return L"Space（空格）";
    case VK_TAB: return L"Tab";
    case VK_RETURN: return L"Enter";
    case VK_ESCAPE: return L"Esc";
    case VK_PAUSE: return L"Pause";
    case VK_MULTIPLY: return L"数字键盘 *";
    case VK_ADD: return L"数字键盘 +";
    case VK_SUBTRACT: return L"数字键盘 -";
    case VK_DECIMAL: return L"数字键盘 .";
    case VK_DIVIDE: return L"数字键盘 /";
    }
    UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
    if (key == VK_INSERT || key == VK_DELETE || key == VK_HOME || key == VK_END || key == VK_PRIOR || key == VK_NEXT ||
        key == VK_UP || key == VK_DOWN || key == VK_LEFT || key == VK_RIGHT || key == VK_DIVIDE) scan |= 0x100;
    wchar_t text[64]{};
    if (GetKeyNameTextW(static_cast<LONG>(scan << 16), text, 64)) return text;
    return L"VK " + std::to_wstring(key);
}
inline std::wstring hotkey_name(const HotkeyConfig& value) {
    if (!value.enabled) return L"已禁用";
    std::wstring text;
    if (value.modifiers & MOD_CONTROL) text += L"Ctrl+";
    if (value.modifiers & MOD_ALT) text += L"Alt+";
    if (value.modifiers & MOD_SHIFT) text += L"Shift+";
    if (value.modifiers & MOD_WIN) text += L"Win+";
    return text + hotkey_key_name(value.key);
}
inline bool valid_hotkey(const HotkeyConfig& value) {
    constexpr UINT allowed = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN;
    if ((value.modifiers & ~allowed) || std::find(hotkey_keys().begin(), hotkey_keys().end(), value.key) == hotkey_keys().end()) return false;
    return !value.enabled || (value.modifiers & (MOD_CONTROL | MOD_ALT | MOD_WIN)) || (value.key >= VK_F1 && value.key <= VK_F24);
}
inline HotkeyConfig load_hotkey(const std::filesystem::path& ini) {
    wchar_t text[64]{}; GetPrivateProfileStringW(L"Keyboard", L"ShowWindow", L"", text, 64, ini.c_str());
    unsigned enabled = 0, modifiers = 0, key = 0; wchar_t trailing = 0;
    if (swscanf_s(text, L"%u,%u,%u%c", &enabled, &modifiers, &key, &trailing, 1u) != 3 || enabled > 1) return {};
    HotkeyConfig value{enabled != 0, modifiers, key}; return valid_hotkey(value) ? value : HotkeyConfig{};
}
inline bool save_hotkey(const std::filesystem::path& ini, const HotkeyConfig& value) {
    // One key avoids partially saving the modifier/key/enabled combination.
    auto text = std::to_wstring(value.enabled) + L"," + std::to_wstring(value.modifiers) + L"," + std::to_wstring(value.key);
    return WritePrivateProfileStringW(L"Keyboard", L"ShowWindow", text.c_str(), ini.c_str()) != FALSE;
}
class GlobalHotkey {
    HWND owner_ = nullptr;
    int id_ = 0;
    HotkeyConfig config_;
    std::wstring failure_;
public:
    using Save = std::function<bool(const HotkeyConfig&)>;
    GlobalHotkey() = default;
    GlobalHotkey(const GlobalHotkey&) = delete;
    GlobalHotkey& operator=(const GlobalHotkey&) = delete;
    ~GlobalHotkey() { stop(); }
    void stop() { if (id_) UnregisterHotKey(owner_, id_); id_ = 0; }
    const HotkeyConfig& config() const { return config_; }
    int id() const { return id_; }
    const std::wstring& failure() const { return failure_; }
    bool handles(WPARAM id) const { return id_ && id == static_cast<WPARAM>(id_); }
    void start(HWND owner, HotkeyConfig value) {
        stop(); owner_ = owner; config_ = value;
        try { change(value); } catch (const Error& error) { failure_ = wide(error.what()); }
    }
    void change(HotkeyConfig next, const Save& save = {}) {
        if (!valid_hotkey(next)) throw Error("请选择 Ctrl、Alt 或 Win 组合；也可以单独使用功能键。" );
        int next_id = id_;
        if (next.enabled && (!id_ || next != config_)) {
            next_id = id_ == 1 ? 2 : 1;
            if (!RegisterHotKey(owner_, next_id, next.modifiers | MOD_NOREPEAT, next.key))
                throw Error("快捷键已被占用或被系统保留，请换一个组合。原设置保持不变。" );
        } else if (!next.enabled) next_id = 0;
        // Acquire the new combination before releasing the old one. A conflict
        // or failed settings write must leave the working shortcut intact.
        bool saved = false;
        try { saved = !save || save(next); }
        catch (...) { if (next_id && next_id != id_) UnregisterHotKey(owner_, next_id); throw; }
        if (!saved) {
            if (next_id && next_id != id_) UnregisterHotKey(owner_, next_id);
            throw Error("无法保存快捷键设置。请检查设置文件是否可写，原设置保持不变。" );
        }
        if (id_ && id_ != next_id) UnregisterHotKey(owner_, id_);
        id_ = next_id; config_ = next; failure_.clear();
    }
};
struct HotkeyDialog {
    HotkeyConfig initial;
    std::wstring initial_failure;
    std::function<void(HotkeyConfig)> apply;
    bool modal = true;
    static constexpr int modifier_ids[]{IDC_HOTKEY_CTRL, IDC_HOTKEY_ALT, IDC_HOTKEY_SHIFT, IDC_HOTKEY_WIN};
    static constexpr UINT modifier_bits[]{MOD_CONTROL, MOD_ALT, MOD_SHIFT, MOD_WIN};
    void enable_controls(HWND dialog) {
        bool enabled = IsDlgButtonChecked(dialog, IDC_HOTKEY_ENABLED) == BST_CHECKED;
        for (auto id : modifier_ids) EnableWindow(GetDlgItem(dialog, id), enabled);
        EnableWindow(GetDlgItem(dialog, IDC_HOTKEY_KEY), enabled);
    }
    void set(HWND dialog, HotkeyConfig value) {
        CheckDlgButton(dialog, IDC_HOTKEY_ENABLED, value.enabled ? BST_CHECKED : BST_UNCHECKED);
        for (size_t i = 0; i < std::size(modifier_ids); ++i) CheckDlgButton(dialog, modifier_ids[i], value.modifiers & modifier_bits[i] ? BST_CHECKED : BST_UNCHECKED);
        auto combo = GetDlgItem(dialog, IDC_HOTKEY_KEY);
        for (LRESULT i = 0, n = SendMessageW(combo, CB_GETCOUNT, 0, 0); i < n; ++i)
            if (SendMessageW(combo, CB_GETITEMDATA, i, 0) == value.key) { SendMessageW(combo, CB_SETCURSEL, i, 0); break; }
        enable_controls(dialog);
    }
    HotkeyConfig get(HWND dialog) {
        HotkeyConfig value; value.enabled = IsDlgButtonChecked(dialog, IDC_HOTKEY_ENABLED) == BST_CHECKED; value.modifiers = 0;
        for (size_t i = 0; i < std::size(modifier_ids); ++i) if (IsDlgButtonChecked(dialog, modifier_ids[i]) == BST_CHECKED) value.modifiers |= modifier_bits[i];
        auto combo = GetDlgItem(dialog, IDC_HOTKEY_KEY); auto index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
        value.key = static_cast<UINT>(SendMessageW(combo, CB_GETITEMDATA, index, 0)); return value;
    }
    void finish(HWND dialog, INT_PTR result) { if (modal) EndDialog(dialog, result); else DestroyWindow(dialog); }
    static INT_PTR CALLBACK procedure(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
        auto self = reinterpret_cast<HotkeyDialog*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (message == WM_INITDIALOG) {
            self = reinterpret_cast<HotkeyDialog*>(lparam); SetWindowLongPtrW(dialog, DWLP_USER, lparam);
            auto combo = GetDlgItem(dialog, IDC_HOTKEY_KEY);
            for (auto key : hotkey_keys()) {
                auto name = hotkey_key_name(key); auto index = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
                SendMessageW(combo, CB_SETITEMDATA, index, key);
            }
            self->set(dialog, self->initial);
            auto text = self->initial_failure.empty() ? L"当前设置：" + hotkey_name(self->initial) + L"。保存后立即生效。" : self->initial_failure;
            SetDlgItemTextW(dialog, IDC_HOTKEY_STATUS, text.c_str()); return TRUE;
        }
        if (!self) return FALSE;
        if (message == WM_CLOSE) { self->finish(dialog, IDCANCEL); return TRUE; }
        if (message != WM_COMMAND) return FALSE;
        switch (LOWORD(wparam)) {
        case IDCANCEL: self->finish(dialog, IDCANCEL); return TRUE;
        case IDOK:
            try { self->apply(self->get(dialog)); self->finish(dialog, IDOK); }
            catch (const std::exception& error) { SetDlgItemTextW(dialog, IDC_HOTKEY_STATUS, wide(error.what()).c_str()); }
            return TRUE;
        case IDC_HOTKEY_DEFAULT: self->set(dialog, {}); break;
        case IDC_HOTKEY_ENABLED: self->enable_controls(dialog); break;
        case IDC_HOTKEY_CTRL: case IDC_HOTKEY_ALT: case IDC_HOTKEY_SHIFT: case IDC_HOTKEY_WIN: break;
        case IDC_HOTKEY_KEY: if (HIWORD(wparam) != CBN_SELCHANGE) return FALSE; break;
        default: return FALSE;
        }
        auto text = L"待保存：" + hotkey_name(self->get(dialog)); SetDlgItemTextW(dialog, IDC_HOTKEY_STATUS, text.c_str()); return TRUE;
    }
};
}
