#pragma once
#include "common.hpp"
#include <commctrl.h>
#include <shellapi.h>
#include <objbase.h>
#include <condition_variable>
#include <deque>
#include <list>

namespace wl {
struct FileIcon { HIMAGELIST images = nullptr; int index = I_IMAGENONE; };
struct IconRequest { std::string path; uint32_t attributes = FILE_ATTRIBUTE_NORMAL; bool type_only = false; };
inline FileIcon load_file_icon(const IconRequest& request) {
    auto name = wide(request.path);
    SHFILEINFOW info{};
    auto images = SHGetFileInfoW(name.c_str(), request.attributes, &info, sizeof(info),
        SHGFI_SYSICONINDEX | SHGFI_SMALLICON | (request.type_only ? SHGFI_USEFILEATTRIBUTES : 0));
    return {reinterpret_cast<HIMAGELIST>(images), images ? info.iIcon : I_IMAGENONE};
}

class AsyncIcons {
public:
    using Loader = std::function<FileIcon(const IconRequest&)>;
    using Clock = std::function<uint64_t()>;
private:
    struct Stamp {
        uint64_t modified = 0, created = 0, size = 0; uint32_t attributes = 0;
        bool operator==(const Stamp&) const = default;
    };
    struct Job { std::string key; uint64_t revision; IconRequest request; };
    struct Done { std::string key; uint64_t revision; FileIcon icon; };
    struct Entry {
        int index = I_IMAGENONE;
        bool pending = false;
        uint64_t revision = 0, retry_after = 0;
        Stamp stamp;
        std::list<std::string>::iterator used;
    };
    struct State {
        std::mutex mutex;
        std::condition_variable work;
        std::deque<Job> jobs;
        std::vector<Done> done;
        HWND window = nullptr;
        UINT message = 0;
        bool stopped = false;
        Loader loader;
        explicit State(Loader load) : loader(std::move(load)) {}
    };
    std::shared_ptr<State> state_;
    // These fields are accessed only by the GUI thread.
    std::unordered_map<std::string, Entry> cache_;
    std::list<std::string> recent_;
    uint64_t revision_ = 0;
    size_t capacity_;
    Clock clock_;
    HIMAGELIST images_ = nullptr;
    static Stamp stamp(const Result& row) { return {row.modified, row.created, row.size, row.attributes}; }
    static std::string file_key(const Result& row) { return "path:" + normalize_path(row.path); }
    int lookup(const std::string& key, const IconRequest& request, const Stamp& version) {
        auto found = cache_.find(key);
        if (found == cache_.end()) {
            recent_.push_front(key);
            Entry entry; entry.stamp = version; entry.used = recent_.begin(); entry.revision = ++revision_;
            found = cache_.emplace(key, std::move(entry)).first;
            if (cache_.size() > capacity_) { cache_.erase(recent_.back()); recent_.pop_back(); }
        } else recent_.splice(recent_.begin(), recent_, found->second.used);
        auto& entry = found->second;
        if (!(entry.stamp == version)) {
            entry.stamp = version; entry.index = I_IMAGENONE; entry.pending = false;
            entry.retry_after = 0; entry.revision = ++revision_;
        }
        if (entry.index != I_IMAGENONE || entry.pending || clock_() < entry.retry_after) return entry.index;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->stopped || !state_->window || state_->jobs.size() >= 256) return I_IMAGENONE;
            entry.pending = true; entry.revision = ++revision_;
            state_->jobs.push_back({key, entry.revision, request});
        }
        state_->work.notify_one(); return I_IMAGENONE;
    }
public:
    explicit AsyncIcons(Loader loader = load_file_icon, size_t capacity = 4096, Clock clock = GetTickCount64)
        : state_(std::make_shared<State>(std::move(loader))), capacity_(std::max<size_t>(2, capacity)), clock_(std::move(clock)) {}
    ~AsyncIcons() { stop(); }
    AsyncIcons(const AsyncIcons&) = delete;
    AsyncIcons& operator=(const AsyncIcons&) = delete;
    void start(HWND window, UINT message) {
        auto state = state_;
        { std::lock_guard lock(state->mutex); state->window = window; state->message = message; }
        // A Shell extension may block indefinitely. The worker owns only this
        // shared state, never Window/controls, so closing never waits for Shell.
        std::thread([state] {
            auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            for (;;) {
                Job job;
                {
                    std::unique_lock lock(state->mutex);
                    state->work.wait(lock, [&] { return state->stopped || !state->jobs.empty(); });
                    if (state->stopped) break;
                    job = std::move(state->jobs.front()); state->jobs.pop_front();
                }
                FileIcon icon;
                try { if (SUCCEEDED(com)) icon = state->loader(job.request); } catch (...) {}
                {
                    std::lock_guard lock(state->mutex);
                    if (state->stopped) break;
                    state->done.push_back({std::move(job.key), job.revision, icon});
                    PostMessageW(state->window, state->message, 0, 0);
                }
            }
            if (SUCCEEDED(com)) CoUninitialize();
        }).detach();
    }
    void stop() {
        { std::lock_guard lock(state_->mutex); state_->stopped = true; state_->window = nullptr; state_->jobs.clear(); state_->done.clear(); }
        state_->work.notify_one();
    }
    int lookup(const std::string& key) {
        bool folder = key == "<folder>";
        return lookup("type:" + key, {folder ? "folder" : "file" + key, static_cast<uint32_t>(folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL), true}, {});
    }
    int lookup(const Result& row) {
        auto type = (row.attributes & FILE_ATTRIBUTE_DIRECTORY) ? "<folder>" : fold(utf8(std::filesystem::path(wide(row.path)).extension().wstring()));
        auto fallback = lookup(type);
        auto actual = lookup(file_key(row), {row.path, row.attributes, false}, stamp(row));
        return actual != I_IMAGENONE ? actual : fallback;
    }
    bool has_file_icon(const Result& row) const {
        auto found = cache_.find(file_key(row));
        return found != cache_.end() && found->second.stamp == stamp(row) && found->second.index != I_IMAGENONE;
    }
    bool collect() {
        std::vector<Done> ready;
        { std::lock_guard lock(state_->mutex); ready.swap(state_->done); }
        bool changed = false;
        for (auto& done : ready) {
            auto found = cache_.find(done.key);
            if (found == cache_.end() || found->second.revision != done.revision) continue;
            auto& entry = found->second;
            entry.pending = false; entry.index = done.icon.index;
            entry.retry_after = entry.index == I_IMAGENONE ? clock_() + 5000 : 0;
            if (done.icon.images) images_ = done.icon.images;
            changed = true;
        }
        return changed;
    }
    HIMAGELIST image_list() const { return images_; }
};
}
