#pragma once
#include "common.hpp"
#include <shlobj.h>
#include <wrl/client.h>

namespace wl {
using Microsoft::WRL::ComPtr;
inline constexpr UINT shell_open_parent = 0x7001, shell_copy_path = 0x7002, shell_rename_selected = 0x7003;
LRESULT shell_menu_message(UINT message, WPARAM wparam, LPARAM lparam, bool& handled);
UINT shell_context(HWND owner, const std::vector<std::wstring>& paths, POINT point);
void shell_verb(HWND owner, const std::vector<std::wstring>& paths, const char* verb, bool shift = false);
void shell_open(HWND owner, const std::wstring& path, bool parent = false);
void clipboard_paths(HWND owner, const std::vector<std::wstring>& paths, bool cut, bool text_only);
void shell_drag(HWND owner, const std::vector<std::wstring>& paths);
void shell_rename(HWND owner, const std::wstring& path, const std::wstring& name);
std::vector<std::string> shell_menu_verbs(HWND owner, const std::vector<std::wstring>& paths);
}
