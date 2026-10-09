#pragma once
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <unordered_map>
#include <vector>

namespace wl {
// All narrow application strings, index names, IPC and redirected text are UTF-8.
// UTF-16 is used only when interfacing with Windows' W/Shell APIs and UI controls.
constexpr uint32_t no_row = UINT32_MAX;
constexpr uint32_t deleted_attribute = 0x80000000;
class Error : public std::runtime_error { public: using std::runtime_error::runtime_error; };
inline void require_utf8_process() {
    if (GetACP() != CP_UTF8) throw Error("WLocate requires the embedded UTF-8 manifest and Windows 10 version 1903 or later");
}
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& h) noexcept : value(std::exchange(h.value, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& h) noexcept { if (this != &h) { reset(); value = std::exchange(h.value, INVALID_HANDLE_VALUE); } return *this; }
    explicit operator bool() const { return value != nullptr && value != INVALID_HANDLE_VALUE; }
    void reset(HANDLE h = INVALID_HANDLE_VALUE) { if (*this) CloseHandle(value); value = h; }
};
std::string utf8(std::wstring_view value);
std::wstring wide(std::string_view value);
std::string fold(std::string_view value);
std::string win_error(DWORD code = GetLastError());
std::wstring module_path();
std::filesystem::path data_directory();
uint64_t filetime_now();
uint64_t filetime_value(FILETIME value);
bool is_admin();
void log(std::string_view text);
bool read_exact(HANDLE file, void* data, size_t size);
bool write_exact(HANDLE file, const void* data, size_t size);
std::string filename(std::string_view path);
std::string directory(std::string_view path);
std::string normalize_path(std::string value);
// Trigram presence filter. Collisions only cause extra checks, never false negatives.
inline uint64_t name_bloom(std::string_view value) {
    uint64_t mask = 0;
    for (size_t i = 2; i < value.size(); ++i) {
        uint32_t gram = uint8_t(value[i - 2]) | (uint32_t(uint8_t(value[i - 1])) << 8) | (uint32_t(uint8_t(value[i])) << 16);
        gram ^= gram >> 11; gram *= 0x9E3779B1u; mask |= 1ull << (gram >> 26);
    }
    return mask;
}

struct Result {
    std::string path;
    uint64_t size = 0, modified = 0, created = 0, accessed = 0;
    uint32_t attributes = 0;
};
enum QueryFlag : uint32_t {
    MatchCase = 1, MatchPath = 2, Regex = 4, WholeWord = 8,
    Basename = 16, AllPatterns = 32, CountOnly = 64, Existing = 128,
    NonExisting = 256, EverythingSyntax = 512
};
enum class Sort : uint32_t { Name, Path, Size, Modified, Created, Extension, None };
struct Request {
    uint32_t operation = 0;
    uint32_t flags = 0;
    uint64_t limit = UINT64_MAX, offset = 0;
    Sort sort = Sort::Name;
    bool descending = false;
    uint32_t timeout_ms = 120000;
    std::vector<std::string> patterns;
};
struct Reply {
    uint64_t total = 0, generation = 0, search_us = 0;
    std::vector<Result> results;
    std::string message;
};
}
