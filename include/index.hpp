#pragma once
#include "query.hpp"
#include <deque>

namespace wl {
struct Entry {
    uint64_t id = 0, parent = 0, size = 0, modified = 0, created = 0, accessed = 0;
    uint32_t attributes = 0, name = 0, next_name = no_row, parent_row = no_row;
};
struct Name {
    std::string text, lower;
    uint32_t first = no_row;
};
struct RawEntry {
    uint64_t id = 0, parent = 0, size = 0, modified = 0, created = 0, accessed = 0;
    uint32_t attributes = 0;
    std::string name;
};
class Volume {
public:
    mutable std::shared_mutex mutex;
    std::string root;
    uint64_t serial = 0, root_id = 0, journal_id = 0, next_usn = 0, generation = 1, name_generation = 1;
    uint64_t checkpoint_usn = 0;
    uint64_t pending_updates = 0;
    uint64_t files = 0, directories = 0;
    bool ready = false;
    std::string state = "indexing";
    std::vector<Entry> entries;
    std::deque<Name> names;
    std::vector<uint32_t> sorted_names;
    std::unordered_map<uint64_t, uint32_t> directory_rows;
    explicit Volume(std::string source) : root(std::move(source)) {}
    void add_initial(RawEntry record);
    void finish();
    void compact();
    void replace_links(uint64_t id, const std::vector<RawEntry>& records);
    std::string full_path(uint32_t row) const;
    Result result(uint32_t row, bool with_path = true) const;
    std::vector<uint32_t> find(const Query& query, uint64_t stop_after, const std::function<bool()>& cancelled, bool ordered = false) const;
    void save(const std::filesystem::path& path) const;
    static std::shared_ptr<Volume> load(const std::filesystem::path& path, std::string root, uint64_t serial);
private:
    std::unordered_map<std::string_view, uint32_t> name_ids_;
    std::vector<uint64_t> name_masks_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> added_ids_;
    // Intrusive child chains use four bytes per row, with one head per parent.
    // Keying heads by NTFS file reference preserves descendants on directory moves.
    std::unordered_map<uint64_t, uint32_t> first_children_;
    std::vector<uint32_t> next_sibling_;
    size_t base_count_ = 0, sorted_name_count_ = 0;
    uint32_t intern(std::string name);
    void add(RawEntry record, bool delta);
    std::vector<uint32_t> rows_for_id(uint64_t id) const;
    uint32_t parent_row(uint32_t row) const;
    void link_child(uint32_t row);
    void unlink_child(uint32_t row);
    std::vector<uint32_t> find_literals(const Query& query, const std::vector<std::string_view>& literals,
        uint64_t stop_after, const std::function<bool()>& cancelled, bool ordered) const;
};
struct RowRef { uint32_t volume, row; };
class Engine {
public:
    mutable std::shared_mutex mutex;
    std::vector<std::shared_ptr<Volume>> volumes;
    Reply search(const Request& request, const std::function<bool()>& cancelled = [] { return false; });
    Reply status() const;
    void set_volumes(std::vector<std::shared_ptr<Volume>> value);
private:
    struct Cached {
        Request request;
        uint64_t generation = 0;
        std::vector<std::shared_ptr<Volume>> volumes;
        std::vector<RowRef> rows;
    };
    std::mutex cache_mutex_;
    std::shared_ptr<Cached> cache_;
    uint64_t topology_ = 1;
};
}
