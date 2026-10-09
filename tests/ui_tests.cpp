#include "app.hpp"
#include <fstream>
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int) {
    int count = 0; auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    auto output = count > 1 ? std::filesystem::path(args[1]) : std::filesystem::temp_directory_path() / L"WLocate-ui-test";
    bool icons = count > 2 && std::wstring_view(args[2]) == L"--file-icons";
    LocalFree(args); std::filesystem::create_directories(output);
    try { return wl::run_gui(instance, SW_SHOWNOACTIVATE, icons ? L"figma.exe" : L"python*.exe", output); }
    catch (const std::exception& error) { std::ofstream file(output / L"failure.txt"); file << error.what(); return 1; }
}
