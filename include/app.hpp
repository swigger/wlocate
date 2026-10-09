#pragma once
#include "ipc.hpp"

namespace wl {
inline constexpr wchar_t service_name[] = L"WLocateService";
int run_service();
int run_server();
void install_service();
void uninstall_service();
int run_gui(HINSTANCE instance, int show, std::wstring initial_query = {}, std::filesystem::path test_output = {});
}
