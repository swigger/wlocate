#include "app.hpp"
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int show) {
    int argc = 0; auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool command = argc > 1 && std::wstring_view(argv[1]).starts_with(L"--");
    try {
        wl::require_utf8_process();
        std::wstring option = argc > 1 ? argv[1] : L"";
        if (option == L"--service") { LocalFree(argv); argv = nullptr; return wl::run_service(); }
        if (option == L"--server") { LocalFree(argv); argv = nullptr; return wl::run_server(); }
        if (option == L"--tray") { LocalFree(argv); argv = nullptr; return wl::run_gui(instance, SW_HIDE); }
        if (option == L"--install-service") wl::install_service();
        else if (option == L"--uninstall-service") wl::uninstall_service();
        else if (option == L"--reindex") { wl::Request request; request.operation = 2; wl::request_service(request); }
        else if (option == L"--version") { auto text = std::string("WLocate 1.0.0 native x64\r\n"); wl::write_exact(GetStdHandle(STD_OUTPUT_HANDLE), text.data(), text.size()); }
        else { std::wstring query; for (int i = 1; i < argc; ++i) { if (i > 1) query += L' '; query += argv[i]; } LocalFree(argv); argv = nullptr; return wl::run_gui(instance, show, query); }
        LocalFree(argv); return 0;
    } catch (const std::exception& error) {
        LocalFree(argv); wl::log(error.what());
        std::string message = std::string("WLocate: ") + error.what() + "\r\n";
        if (command) wl::write_exact(GetStdHandle(STD_ERROR_HANDLE), message.data(), message.size());
        else MessageBoxW(nullptr, wl::wide(error.what()).c_str(), L"WLocate", MB_OK | MB_ICONERROR);
        return 2;
    }
}
