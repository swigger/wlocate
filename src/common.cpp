#include "common.hpp"
#include <shlobj.h>
#include <fstream>
#include <utility>

namespace wl {
std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw Error("Cannot encode Unicode text");
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::wstring wide(std::string_view value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) throw Error("Invalid UTF-8 text");
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}
std::string fold(std::string_view value) {
    bool ascii = std::all_of(value.begin(), value.end(), [](unsigned char c) { return c < 128; });
    if (ascii) {
        std::string result(value);
        for (char& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        return result;
    }
    auto text = wide(value);
    int size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr, 0);
    if (!size) return std::string(value);
    std::wstring lower(size, L'\0');
    LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), static_cast<int>(text.size()), lower.data(), size, nullptr, nullptr, 0);
    return utf8(lower);
}
std::string win_error(DWORD code) {
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string result = text ? utf8(text) : "Windows error " + std::to_string(code);
    if (text) LocalFree(text);
    while (!result.empty() && (result.back() == '\r' || result.back() == '\n')) result.pop_back();
    return result;
}
std::wstring module_path() { std::wstring path(32768, L'\0'); path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()))); return path; }
std::filesystem::path data_directory() {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &path))) throw Error("Cannot locate ProgramData");
    std::filesystem::path result(path); CoTaskMemFree(path); return result / L"WLocate";
}
uint64_t filetime_value(FILETIME value) { return (uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime; }
uint64_t filetime_now() { FILETIME value; GetSystemTimeAsFileTime(&value); return filetime_value(value); }
bool is_admin() {
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID sid = nullptr; BOOL member = FALSE;
    if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &sid)) {
        CheckTokenMembership(nullptr, sid, &member); FreeSid(sid);
    }
    return member != FALSE;
}
void log(std::string_view text) {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    try {
        auto root = data_directory(); std::filesystem::create_directories(root);
        auto path = root / L"service.log";
        if (std::filesystem::exists(path) && std::filesystem::file_size(path) > 8 * 1024 * 1024) {
            std::error_code ec; std::filesystem::remove(root / L"service.previous.log", ec);
            std::filesystem::rename(path, root / L"service.previous.log", ec);
        }
        SYSTEMTIME now; GetLocalTime(&now);
        char stamp[64]; snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u ", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        std::ofstream file(path, std::ios::app | std::ios::binary); file << stamp << text << '\n';
    } catch (...) { }
}
bool read_exact(HANDLE file, void* data, size_t size) {
    auto p = static_cast<char*>(data);
    while (size) { DWORD got = 0; if (!ReadFile(file, p, static_cast<DWORD>(std::min<size_t>(size, 1 << 20)), &got, nullptr) || !got) return false; p += got; size -= got; }
    return true;
}
bool write_exact(HANDLE file, const void* data, size_t size) {
    auto p = static_cast<const char*>(data);
    while (size) { DWORD done = 0; if (!WriteFile(file, p, static_cast<DWORD>(std::min<size_t>(size, 1 << 20)), &done, nullptr) || !done) return false; p += done; size -= done; }
    return true;
}
std::string filename(std::string_view path) { auto pos = path.find_last_of("\\/"); return std::string(pos == path.npos ? path : path.substr(pos + 1)); }
std::string directory(std::string_view path) { auto pos = path.find_last_of("\\/"); if (pos == path.npos) return {}; if (pos == 2 && path[1] == ':') ++pos; return std::string(path.substr(0, pos)); }
std::string normalize_path(std::string value) { std::replace(value.begin(), value.end(), '/', '\\'); return value; }
}
