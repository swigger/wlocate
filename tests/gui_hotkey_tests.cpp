// Test our settings dialog and real RegisterHotKey ownership without sending
// keyboard input to the desktop or modifying the user's settings.
#include "../src/gui.cpp"
#include <iostream>

int main(int argc, char** argv) {
    unsigned checks = 0;
    try {
        wl::require_utf8_process(); OleInitialize(nullptr);
        auto output = argc > 1 ? std::filesystem::path(wl::wide(argv[1])) : std::filesystem::temp_directory_path() / (L"wlocate-hotkey-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(output); auto ini = output / L"settings.ini";
        auto require = [&](bool value, const char* message) { ++checks; if (!value) throw wl::Error(message); };
        auto rejects = [&](auto action, const char* message) { bool failed = false; try { action(); } catch (const wl::Error&) { failed = true; } require(failed, message); };
        auto instance = GetModuleHandleW(nullptr);
        auto owner = CreateWindowExW(0, L"STATIC", L"WLocate hotkey tests", WS_OVERLAPPEDWINDOW, 50, 50, 700, 400, nullptr, nullptr, instance, nullptr);
        auto blocker = CreateWindowExW(0, L"STATIC", L"WLocate hotkey conflict", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
        require(owner && blocker, "test windows created");
        std::vector<wl::HotkeyConfig> available;
        for (UINT key = VK_F24; key >= VK_F13 && available.size() < 3; --key) {
            wl::HotkeyConfig config{true, MOD_CONTROL | MOD_ALT | MOD_SHIFT, key};
            if (RegisterHotKey(blocker, 100, config.modifiers | MOD_NOREPEAT, key)) { UnregisterHotKey(blocker, 100); available.push_back(config); }
        }
        require(available.size() == 3, "three unused test key combinations available");
        auto a = available[0], b = available[1], c = available[2];
        WritePrivateProfileStringW(L"Keyboard", L"ShowWindow", nullptr, ini.c_str());
        require(wl::load_hotkey(ini) == wl::HotkeyConfig{}, "old settings default to Ctrl+Alt+L");
        require(wl::save_hotkey(ini, a) && wl::load_hotkey(ini) == a, "custom shortcut persists and reloads");
        WritePrivateProfileStringW(L"Keyboard", L"ShowWindow", L"1,65535,0garbage", ini.c_str());
        require(wl::load_hotkey(ini) == wl::HotkeyConfig{}, "invalid saved shortcut falls back safely");
        require(!wl::valid_hotkey({true, 0, 'L'}) && wl::valid_hotkey({true, 0, VK_F9}), "bare typing keys rejected, function keys accepted");
        require(wl::valid_hotkey({true, MOD_WIN, VK_SPACE}) && !wl::valid_hotkey({true, MOD_CONTROL, VK_F12}), "Win combinations supported and F12 excluded");
        wl::GlobalHotkey hotkey; hotkey.start(owner, a);
        require(hotkey.id() && hotkey.failure().empty(), "initial shortcut registered");
        auto occupied = [&](wl::HotkeyConfig value) {
            bool registered = RegisterHotKey(blocker, 101, value.modifiers | MOD_NOREPEAT, value.key) != FALSE;
            if (registered) UnregisterHotKey(blocker, 101);
            return !registered;
        };
        require(occupied(a), "Windows confirms ownership of initial shortcut");
        auto save = [&](const wl::HotkeyConfig& value) { return wl::save_hotkey(ini, value); };
        hotkey.change(b, save);
        require(!occupied(a) && occupied(b) && wl::load_hotkey(ini) == b, "change releases old shortcut and immediately reserves saved new shortcut");
        auto registered_id = hotkey.id(); hotkey.change(b, save);
        require(hotkey.id() == registered_id && occupied(b), "unchanged settings do not duplicate registration");
        require(RegisterHotKey(blocker, 102, c.modifiers | MOD_NOREPEAT, c.key), "competing shortcut registered");
        rejects([&] { hotkey.change(c, save); }, "occupied shortcut rejected");
        require(hotkey.config() == b && occupied(b) && wl::load_hotkey(ini) == b, "conflict retains current shortcut and saved settings");
        UnregisterHotKey(blocker, 102);
        rejects([&] { hotkey.change(c, [](const auto&) { return false; }); }, "failed settings write reported");
        require(occupied(b) && !occupied(c) && hotkey.config() == b, "failed save rolls back candidate registration");
        auto disabled = b; disabled.enabled = false; hotkey.change(disabled, save);
        require(!hotkey.id() && !occupied(b) && wl::load_hotkey(ini) == disabled, "disable releases shortcut and persists");
        hotkey.start(owner, wl::load_hotkey(ini)); require(!hotkey.id(), "disabled shortcut stays disabled after restart");
        hotkey.change(b, save); hotkey.stop(); hotkey.start(owner, wl::load_hotkey(ini));
        require(hotkey.id() && occupied(b), "custom shortcut restored on restart");
        hotkey.stop(); RegisterHotKey(blocker, 102, b.modifiers | MOD_NOREPEAT, b.key); hotkey.start(owner, b);
        require(!hotkey.id() && !hotkey.failure().empty() && hotkey.config() == b, "startup conflict is visible and retains configured combination");
        UnregisterHotKey(blocker, 102); hotkey.change(b, save);
        require(hotkey.id() && hotkey.failure().empty(), "same setting can retry after startup conflict clears");

        unsigned applies = 0;
        wl::HotkeyDialog model{b, {}, [&](wl::HotkeyConfig value) { hotkey.change(value, save); ++applies; }, false};
        auto create = [&] {
            auto dialog = CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_HOTKEY), owner, wl::HotkeyDialog::procedure, reinterpret_cast<LPARAM>(&model));
            require(dialog != nullptr, "settings dialog resource loads"); return dialog;
        };
        auto dialog = create();
        require(model.get(dialog) == b, "dialog shows currently configured shortcut");
        SendMessageW(dialog, WM_COMMAND, IDC_HOTKEY_DEFAULT, 0);
        require(model.get(dialog) == wl::HotkeyConfig{} && hotkey.config() == b, "restore default edits only the pending selection");
        ShowWindow(dialog, SW_SHOWNOACTIVATE); UpdateWindow(dialog); wl::render_window(dialog, output / L"hotkey-settings.png");
        SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
        require(!IsWindow(dialog) && applies == 0 && wl::load_hotkey(ini) == b, "cancel leaves active shortcut and disk settings unchanged");
        dialog = create(); model.set(dialog, c);
        require(RegisterHotKey(blocker, 102, c.modifiers | MOD_NOREPEAT, c.key), "dialog conflict fixture acquired");
        SendMessageW(dialog, WM_COMMAND, IDOK, 0);
        require(IsWindow(dialog) && hotkey.config() == b && wl::window_text(GetDlgItem(dialog, IDC_HOTKEY_STATUS)).find(L"占用") != std::wstring::npos,
            "conflict keeps dialog open with actionable error");
        ShowWindow(dialog, SW_SHOWNOACTIVATE); UpdateWindow(dialog); wl::render_window(dialog, output / L"hotkey-conflict.png");
        UnregisterHotKey(blocker, 102); SendMessageW(dialog, WM_COMMAND, IDOK, 0);
        require(!IsWindow(dialog) && hotkey.config() == c && wl::load_hotkey(ini) == c && applies == 1, "dialog OK saves and applies chosen shortcut");
        model.initial = c; dialog = create();
        CheckDlgButton(dialog, IDC_HOTKEY_ENABLED, BST_UNCHECKED); SendMessageW(dialog, WM_COMMAND, IDC_HOTKEY_ENABLED, 0);
        require(!IsWindowEnabled(GetDlgItem(dialog, IDC_HOTKEY_KEY)) && !IsWindowEnabled(GetDlgItem(dialog, IDC_HOTKEY_WIN)), "disable checkbox disables key controls");
        SendMessageW(dialog, WM_COMMAND, IDOK, 0);
        require(!hotkey.id() && !wl::load_hotkey(ini).enabled, "dialog can disable global shortcut");
        hotkey.stop(); DestroyWindow(blocker); DestroyWindow(owner); OleUninitialize();
        std::cout << checks << " hotkey checks passed; renders: " << wl::utf8(output.wstring()) << '\n'; return 0;
    } catch (const std::exception& error) { std::cerr << "After " << checks << " checks: " << error.what() << '\n'; return 1; }
}
