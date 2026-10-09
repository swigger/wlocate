#include "app.hpp"
#include "ntfs.hpp"
#include <iostream>
#include <map>
#include <shlobj.h>
#include <aclapi.h>
#include <sddl.h>

namespace wl {
namespace {
SERVICE_STATUS_HANDLE status_handle = nullptr;
SERVICE_STATUS service_status{};
HANDLE stop_handle = nullptr;
void report(DWORD state, DWORD error = NO_ERROR) {
    service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS; service_status.dwCurrentState = state;
    service_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    service_status.dwWin32ExitCode = error; service_status.dwWaitHint = state == SERVICE_STOP_PENDING ? 120000 : 0;
    if (status_handle) SetServiceStatus(status_handle, &service_status);
}
DWORD WINAPI control(DWORD code, DWORD, void*, void*) {
    if (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_SHUTDOWN) { report(SERVICE_STOP_PENDING); SetEvent(stop_handle); }
    return NO_ERROR;
}
BOOL WINAPI console_control(DWORD code) {
    if (code == CTRL_C_EVENT || code == CTRL_BREAK_EVENT || code == CTRL_CLOSE_EVENT) { if (stop_handle) SetEvent(stop_handle); return TRUE; }
    return FALSE;
}
void background(HANDLE stop) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    HANDLE token_handle = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token_handle)) {
        Handle token(token_handle); TOKEN_PRIVILEGES privilege{}; privilege.PrivilegeCount = 1;
        if (LookupPrivilegeValueW(nullptr, SE_BACKUP_NAME, &privilege.Privileges[0].Luid)) {
            privilege.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(token.value, FALSE, &privilege, 0, nullptr, nullptr);
        }
    }
    Engine engine;
    std::map<wchar_t, std::shared_ptr<Volume>> current;
    std::mutex publication;
    std::atomic<uint64_t> rebuild_epoch = 0;
    auto publish = [&] {
        std::vector<std::shared_ptr<Volume>> volumes;
        for (const auto& [drive, volume] : current) volumes.push_back(volume);
        engine.set_volumes(std::move(volumes));
    };
    auto index_volume = [&](wchar_t drive, std::stop_token token) {
        auto root = utf8(std::wstring(1, drive) + L":\\");
        auto cancelled = [&] { return token.stop_requested() || WaitForSingleObject(stop, 0) == WAIT_OBJECT_0; };
        uint64_t epoch = rebuild_epoch.load(); bool force = false;
        while (!cancelled()) {
            try {
                NtfsReader reader(drive);
                auto snapshot = data_directory() / (std::wstring(1, drive) + L".widx");
                std::shared_ptr<Volume> volume;
                if (!force) {
                    try {
                        volume = Volume::load(snapshot, root, reader.serial());
                        if (volume) { while (reader.poll(*volume)) if (cancelled()) return; log(volume->root + " restored from snapshot"); }
                    } catch (const std::exception& error) { log(std::string("Snapshot rebuild: ") + error.what()); volume.reset(); }
                }
                if (!volume) volume = reader.scan(cancelled);
                {
                    std::lock_guard guard(publication);
                    if (cancelled()) return;
                    current[drive] = volume; publish();
                }
                if (cancelled()) return;
                volume->save(snapshot);
                auto saved = GetTickCount64();
                while (!cancelled() && epoch == rebuild_epoch.load()) {
                    while (reader.poll(*volume)) if (cancelled() || epoch != rebuild_epoch.load()) break;
                    if (GetTickCount64() - saved > 120000) {
                        // Compact accumulated journal tombstones before the next durable snapshot.
                        {
                            std::unique_lock lock(volume->mutex);
                            auto live = volume->files + volume->directories;
                            if (volume->entries.size() > live + std::max<uint64_t>(10000, live / 20)) {
                                volume->compact();
                            }
                        }
                        volume->save(snapshot); saved = GetTickCount64();
                    }
                    WaitForSingleObject(stop, 250);
                }
                if (cancelled()) { volume->save(snapshot); return; }
                epoch = rebuild_epoch.load(); force = true;
                log(volume->root + " requested rebuild");
            } catch (const std::exception& error) {
                if (cancelled()) return;
                auto message = std::string(error.what()); log(root + " index error: " + message);
                {
                    std::lock_guard guard(publication); if (cancelled()) return;
                    auto failed = std::make_shared<Volume>(root);
                    failed->state = "rebuilding: " + message; current[drive] = failed; publish();
                }
                force = true;
                for (unsigned retry = 0; retry < 20 && !cancelled(); ++retry) WaitForSingleObject(stop, 250);
            }
        }
    };
    std::jthread monitor([&](std::stop_token token) {
        std::map<wchar_t, std::jthread> indexers;
        while (!token.stop_requested() && WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) {
            const auto drives = ntfs_drives();
            for (auto it = indexers.begin(); it != indexers.end();) {
                if (std::find(drives.begin(), drives.end(), it->first) != drives.end()) { ++it; continue; }
                it->second.request_stop();
                { std::lock_guard guard(publication); current.erase(it->first); publish(); }
                log(std::string(1, char(it->first)) + ": volume removed");
                it = indexers.erase(it);
            }
            for (auto drive : drives) if (!indexers.contains(drive)) {
                { std::lock_guard guard(publication); current[drive] = std::make_shared<Volume>(utf8(std::wstring(1, drive) + L":\\")); publish(); }
                indexers.emplace(drive, std::jthread([&, drive](std::stop_token child) { index_volume(drive, child); }));
            }
            WaitForSingleObject(stop, 2000);
        }
        for (auto& [drive, thread] : indexers) thread.request_stop();
    });
    try { serve(engine, stop, [&] { ++rebuild_epoch; }); }
    catch (...) { SetEvent(stop); throw; }
}
void WINAPI service_main(DWORD, wchar_t**) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); stop_handle = event.value;
    status_handle = RegisterServiceCtrlHandlerExW(service_name, control, nullptr);
    if (!status_handle) return;
    report(SERVICE_RUNNING);
    try { background(event.value); report(SERVICE_STOPPED); }
    catch (const std::exception& error) { log(error.what()); report(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR); }
    stop_handle = nullptr;
}
struct ScHandle {
    SC_HANDLE value;
    ~ScHandle() { if (value) CloseServiceHandle(value); }
};
void protect_directory(const std::filesystem::path& directory) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)", SDDL_REVISION_1, &descriptor, nullptr)) throw Error(win_error());
    BOOL present, defaulted; PACL acl = nullptr; GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted);
    auto error = SetNamedSecurityInfoW(const_cast<LPWSTR>(directory.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
    LocalFree(descriptor); if (error != ERROR_SUCCESS) throw Error("Protect WLocate directory: " + win_error(error));
}
void wait_stopped(SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status{}; DWORD needed;
    auto deadline = GetTickCount64() + 120000;
    while (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &needed)) {
        if (status.dwCurrentState == SERVICE_STOPPED) return;
        if (GetTickCount64() > deadline) throw Error("Timed out waiting for WLocate service to stop");
        Sleep(100);
    }
    throw Error("Query service: " + win_error());
}
}
int run_service() {
    SERVICE_TABLE_ENTRYW table[]{{const_cast<wchar_t*>(service_name), service_main}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) throw Error("Service dispatcher: " + win_error());
    return 0;
}
int run_server() {
    if (!is_admin()) throw Error("The indexing server requires administrator privileges");
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); stop_handle = event.value;
    SetConsoleCtrlHandler(console_control, TRUE);
    background(event.value); stop_handle = nullptr; return 0;
}
void install_service() {
    if (!is_admin()) throw Error("Service installation requires administrator privileges");
    std::filesystem::create_directories(data_directory());
    protect_directory(data_directory());
    ScHandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE)};
    if (!manager.value) throw Error(win_error());
    wchar_t* program_files = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &program_files))) throw Error("Cannot locate Program Files");
    auto installed = std::filesystem::path(program_files) / L"WLocate"; CoTaskMemFree(program_files);
    std::filesystem::create_directories(installed);
    protect_directory(installed);
    auto target = installed / L"wlocate.exe";
    auto command = L"\"" + target.wstring() + L"\" --service";
    ScHandle service{OpenServiceW(manager.value, service_name, SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS)};
    if (service.value) {
        SERVICE_STATUS status{}; ControlService(service.value, SERVICE_CONTROL_STOP, &status); wait_stopped(service.value);
    }
    if (_wcsicmp(module_path().c_str(), target.c_str())) {
        std::filesystem::copy_file(module_path(), target, std::filesystem::copy_options::overwrite_existing);
        auto cli = std::filesystem::path(module_path()).parent_path() / L"locate.exe";
        if (std::filesystem::exists(cli)) std::filesystem::copy_file(cli, installed / L"locate.exe", std::filesystem::copy_options::overwrite_existing);
    }
    if (service.value) {
        if (!ChangeServiceConfigW(service.value, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, L"WLocate File Index")) throw Error(win_error());
    } else {
        service.value = CreateServiceW(manager.value, service_name, L"WLocate File Index", SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_QUERY_STATUS,
            SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
        if (!service.value) throw Error("Create service: " + win_error());
    }
    SERVICE_DESCRIPTIONW description{const_cast<wchar_t*>(L"Indexes local NTFS file names and tracks changes for WLocate and locate. No file contents are indexed.")};
    ChangeServiceConfig2W(service.value, SERVICE_CONFIG_DESCRIPTION, &description);
    SC_ACTION actions[]{{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 15000}, {SC_ACTION_RESTART, 60000}};
    SERVICE_FAILURE_ACTIONSW failures{86400, nullptr, nullptr, 3, actions}; ChangeServiceConfig2W(service.value, SERVICE_CONFIG_FAILURE_ACTIONS, &failures);
    if (!StartServiceW(service.value, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) throw Error("Start service: " + win_error());
    log("Service installed at " + utf8(module_path()));
}
void uninstall_service() {
    if (!is_admin()) throw Error("Service removal requires administrator privileges");
    ScHandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)}; if (!manager.value) throw Error(win_error());
    ScHandle service{OpenServiceW(manager.value, service_name, DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS)}; if (!service.value) throw Error(win_error());
    SERVICE_STATUS status{}; ControlService(service.value, SERVICE_CONTROL_STOP, &status); wait_stopped(service.value);
    if (!DeleteService(service.value)) throw Error(win_error());
}
}
