#include "../src/gui_icons.hpp"
#include <iostream>

int main() {
    wl::require_utf8_process(); CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto hwnd = CreateWindowW(L"STATIC", L"Icon cache test", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    unsigned checks = 0, failures = 0;
    auto check = [&](bool ok, const char* what) { ++checks; if (!ok) ++failures; std::cout << (ok ? "PASS " : "FAIL ") << what << '\n'; };
    auto until = [](auto condition) {
        auto deadline = GetTickCount64() + 5000;
        for (;;) { if (condition()) return true; if (GetTickCount64() >= deadline) return false; Sleep(1); }
    };
    struct Provider {
        std::mutex mutex;
        std::unordered_map<std::string, unsigned> calls;
        wl::Handle entered{CreateEventW(nullptr, TRUE, FALSE, nullptr)}, release{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        std::atomic<bool> block = false, fail = false;
        unsigned count(const std::string& key) { std::lock_guard lock(mutex); return calls[key]; }
    };
    auto provider = std::make_shared<Provider>();
    uint64_t now = 0;
    wl::AsyncIcons cache([provider](const wl::IconRequest& request) {
        unsigned call;
        { std::lock_guard lock(provider->mutex); call = ++provider->calls[request.path]; }
        if (request.type_only) return wl::FileIcon{nullptr, 10};
        if (provider->block.exchange(false)) { SetEvent(provider->entered.value); WaitForSingleObject(provider->release.value, 5000); }
        if (provider->fail) return wl::FileIcon{};
        int base = request.path.find("one") != std::string::npos ? 100 : 200;
        return wl::FileIcon{nullptr, base + static_cast<int>(call)};
    }, 4, [&] { return now; });
    cache.start(hwnd, WM_APP + 4);
    cache.lookup(".exe");
    check(until([&] { cache.collect(); return cache.lookup(".exe") == 10; }), "generic type icon becomes available first");
    wl::Result one; one.path = "C:\\one\\Same.exe"; one.modified = 1; one.attributes = FILE_ATTRIBUTE_NORMAL;
    wl::Result two = one; two.path = "C:\\two\\Same.exe";
    provider->block = true;
    check(cache.lookup(one) == 10, "uncached file immediately returns its type fallback");
    check(WaitForSingleObject(provider->entered.value, 1000) == WAIT_OBJECT_0, "real-file icon request was queued");
    for (unsigned i = 0; i < 100; ++i) cache.lookup(one);
    check(provider->count(one.path) == 1, "repaints deduplicate a pending full-path lookup");
    SetEvent(provider->release.value);
    check(until([&] { cache.collect(); return cache.has_file_icon(one); }) && cache.lookup(one) == 101, "actual file icon replaces the fallback");
    cache.lookup(two);
    check(until([&] { cache.collect(); return cache.has_file_icon(two); }) && cache.lookup(two) == 201, "same basename and extension in another directory get an independent icon");
    for (unsigned i = 0; i < 100; ++i) { cache.lookup(one); cache.lookup(two); }
    check(provider->count(one.path) == 1 && provider->count(two.path) == 1, "repeated searches reuse the full-path cache");
    ++one.modified; cache.lookup(one);
    check(until([&] { cache.collect(); return cache.has_file_icon(one); }) && cache.lookup(one) == 102, "changed file metadata refreshes its icon");
    ResetEvent(provider->entered.value); ResetEvent(provider->release.value); provider->block = true;
    ++one.modified; cache.lookup(one);
    check(WaitForSingleObject(provider->entered.value, 1000) == WAIT_OBJECT_0, "old file revision is loading");
    ++one.modified; cache.lookup(one); SetEvent(provider->release.value);
    check(until([&] { cache.collect(); return cache.has_file_icon(one); }) && cache.lookup(one) == 104, "late completion from an older revision cannot overwrite the newer icon");
    provider->fail = true; ++two.modified; cache.lookup(two);
    until([&] { return provider->count(two.path) == 2; });
    check(until([&] { return cache.collect(); }) && cache.lookup(two) == 10, "failed lookup preserves the type fallback");
    for (unsigned i = 0; i < 100; ++i) cache.lookup(two);
    check(provider->count(two.path) == 2, "failed lookups are throttled instead of retried on every repaint");
    provider->fail = false; now = 5001; cache.lookup(two);
    check(until([&] { cache.collect(); return cache.has_file_icon(two); }) && cache.lookup(two) == 203, "failed lookups can recover after the retry interval");
    auto three = one; three.path = "C:\\three\\Same.exe";
    auto four = one; four.path = "C:\\four\\Same.exe";
    cache.lookup(three); until([&] { cache.collect(); return cache.has_file_icon(three); });
    cache.lookup(four); until([&] { cache.collect(); return cache.has_file_icon(four); });
    cache.lookup(one);
    check(until([&] { cache.collect(); return cache.has_file_icon(one); }) && provider->count(one.path) == 5, "bounded LRU cache reloads evicted paths");
    cache.stop();

    wl::AsyncIcons real; real.start(hwnd, WM_APP + 5);
    wl::Result executable; executable.path = wl::utf8(wl::module_path()); executable.attributes = FILE_ATTRIBUTE_NORMAL;
    real.lookup(executable);
    check(until([&] { real.collect(); return real.has_file_icon(executable); }), "real Shell lookup loads this executable's embedded icon asynchronously");
    auto expected = wl::load_file_icon({executable.path, executable.attributes, false});
    check(real.lookup(executable) == expected.index && expected.index != real.lookup(".exe"), "cached executable icon matches its full-path Shell icon and differs from generic EXE");
    real.stop(); DestroyWindow(hwnd); CoUninitialize();
    std::cout << checks << " checks, " << failures << " failures\n"; return failures ? 1 : 0;
}
