#include "index.hpp"
#include <sstream>
#include <array>

namespace wl {
void Volume::link_child(uint32_t row) {
    const auto& entry = entries[row];
    if (entry.id == root_id || (entry.attributes & deleted_attribute)) return;
    auto [head, added] = first_children_.try_emplace(entry.parent, no_row);
    next_sibling_[row] = head->second; head->second = row;
}
void Volume::unlink_child(uint32_t row) {
    auto head = first_children_.find(entries[row].parent);
    if (head == first_children_.end()) return;
    auto* link = &head->second;
    while (*link != no_row && *link != row) link = &next_sibling_[*link];
    if (*link == row) *link = next_sibling_[row];
    next_sibling_[row] = no_row;
    if (head->second == no_row) first_children_.erase(head);
}
uint32_t Volume::intern(std::string name) {
    if (auto found = name_ids_.find(name); found != name_ids_.end()) return found->second;
    auto id = static_cast<uint32_t>(names.size());
    names.push_back({ std::move(name), {}, no_row });
    names.back().lower = fold(names.back().text);
    name_masks_.push_back(name_bloom(names.back().lower));
    name_ids_.emplace(names.back().text, id);
    return id;
}
void Volume::add(RawEntry record, bool delta) {
    auto name = intern(std::move(record.name));
    auto row = static_cast<uint32_t>(entries.size());
    entries.push_back({record.id, record.parent, record.size, record.modified, record.created, record.accessed,
        record.attributes, name, names[name].first});
    next_sibling_.push_back(no_row); link_child(row);
    auto parent = directory_rows.find(record.parent); if (parent != directory_rows.end()) entries.back().parent_row = parent->second;
    names[name].first = row;
    if (record.attributes & FILE_ATTRIBUTE_DIRECTORY) { directory_rows[record.id] = row; ++directories; } else ++files;
    if (delta) added_ids_[record.id].push_back(row);
}
void Volume::add_initial(RawEntry record) { add(std::move(record), false); }
void Volume::finish() {
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.id < b.id; });
    directory_rows.clear(); files = directories = 0;
    first_children_.clear(); next_sibling_.assign(entries.size(), no_row);
    for (auto& name : names) name.first = no_row;
    for (uint32_t row = 0; row < entries.size(); ++row) {
        auto& entry = entries[row];
        entry.next_name = names[entry.name].first; names[entry.name].first = row;
        if (entry.attributes & deleted_attribute) continue;
        link_child(row);
        if (entry.attributes & FILE_ATTRIBUTE_DIRECTORY) { directory_rows[entry.id] = row; ++directories; } else ++files;
    }
    for (auto& entry : entries) { auto parent = directory_rows.find(entry.parent); entry.parent_row = parent == directory_rows.end() ? no_row : parent->second; }
    sorted_names.resize(names.size());
    for (uint32_t i = 0; i < names.size(); ++i) sorted_names[i] = i;
    std::sort(sorted_names.begin(), sorted_names.end(), [&](uint32_t a, uint32_t b) { return names[a].lower < names[b].lower; });
    base_count_ = entries.size(); sorted_name_count_ = names.size(); added_ids_.clear();
    ready = true; state = "ready"; ++generation; ++name_generation;
}
void Volume::compact() {
    std::erase_if(entries, [](const Entry& value) { return value.attributes & deleted_attribute; });
    // Discard unused interned strings as well as rows, so sustained file churn
    // does not grow the name table indefinitely.
    name_ids_.clear(); name_masks_.clear();
    auto previous = std::move(names); names.clear();
    for (auto& entry : entries) entry.name = intern(previous[entry.name].text);
    finish();
}
std::vector<uint32_t> Volume::rows_for_id(uint64_t id) const {
    std::vector<uint32_t> rows;
    auto end = entries.begin() + base_count_;
    auto first = std::lower_bound(entries.begin(), end, id, [](const Entry& e, uint64_t v) { return e.id < v; });
    while (first != end && first->id == id) { rows.push_back(static_cast<uint32_t>(first - entries.begin())); ++first; }
    if (auto found = added_ids_.find(id); found != added_ids_.end()) rows.insert(rows.end(), found->second.begin(), found->second.end());
    return rows;
}
void Volume::replace_links(uint64_t id, const std::vector<RawEntry>& records) {
    auto previous = rows_for_id(id);
    std::erase_if(previous, [&](uint32_t row) { return (entries[row].attributes & deleted_attribute) != 0; });
    bool unchanged_names = previous.size() == records.size();
    std::vector<std::pair<uint32_t, const RawEntry*>> matched;
    if (unchanged_names) for (auto& record : records) {
        auto found = std::find_if(previous.begin(), previous.end(), [&](uint32_t row) {
            const auto& old = entries[row];
            return old.parent == record.parent && names[old.name].text == record.name && (old.attributes & FILE_ATTRIBUTE_DIRECTORY) == (record.attributes & FILE_ATTRIBUTE_DIRECTORY);
        });
        if (found == previous.end()) { unchanged_names = false; break; }
        matched.emplace_back(*found, &record);
    }
    if (unchanged_names) {
        bool changed = false;
        for (auto [row, record] : matched) {
            auto& old = entries[row];
            changed |= old.size != record->size || old.modified != record->modified || old.created != record->created || old.accessed != record->accessed || old.attributes != record->attributes;
            old.size = record->size; old.modified = record->modified; old.created = record->created; old.accessed = record->accessed; old.attributes = record->attributes;
        }
        if (changed) ++generation;
        return;
    }
    // Preserve directory row identity on rename/move, so every descendant's
    // parent-row cache stays valid without rewriting the whole subtree.
    if (previous.size() == 1 && records.size() == 1 && (entries[previous[0]].attributes & FILE_ATTRIBUTE_DIRECTORY) && (records[0].attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        auto row = previous[0]; auto& old = entries[row]; const auto& record = records[0];
        uint32_t* link = &names[old.name].first;
        while (*link != no_row && *link != row) link = &entries[*link].next_name;
        if (*link == row) *link = old.next_name;
        if (old.parent != record.parent) unlink_child(row);
        old.name = intern(record.name); old.next_name = names[old.name].first; names[old.name].first = row;
        bool moved = old.parent != record.parent;
        old.parent = record.parent; auto parent = directory_rows.find(record.parent); old.parent_row = parent == directory_rows.end() ? no_row : parent->second;
        if (moved) link_child(row);
        old.size = record.size; old.modified = record.modified; old.created = record.created; old.accessed = record.accessed; old.attributes = record.attributes;
        ++generation; ++name_generation; return;
    }
    for (auto row : previous) {
        auto& entry = entries[row];
        if (entry.attributes & deleted_attribute) continue;
        if (entry.attributes & FILE_ATTRIBUTE_DIRECTORY) --directories; else --files;
        unlink_child(row);
        entry.attributes |= deleted_attribute;
    }
    directory_rows.erase(id); added_ids_.erase(id);
    for (auto& record : records) add(record, true);
    ++generation; ++name_generation;
}
uint32_t Volume::parent_row(uint32_t row) const {
    const auto& entry = entries[row]; auto parent = entry.parent_row;
    if (parent < entries.size() && entries[parent].id == entry.parent && !(entries[parent].attributes & deleted_attribute)) return parent;
    auto found = directory_rows.find(entry.parent); return found == directory_rows.end() ? no_row : found->second;
}
std::string Volume::full_path(uint32_t row) const {
    std::array<std::string_view, 512> parts; size_t count = 0;
    auto current = row;
    while (current < entries.size()) {
        const auto& entry = entries[current];
        if (entry.attributes & deleted_attribute) return {};
        if (entry.id == root_id) break;
        if (count == parts.size()) return {};
        parts[count++] = names[entry.name].text;
        auto parent = parent_row(current); if (parent == no_row || parent == current) return {};
        current = parent;
    }
    if (current >= entries.size()) return {};
    std::string path = root;
    while (count) { if (!path.ends_with('\\')) path += '\\'; path += parts[--count]; }
    return path;
}
Result Volume::result(uint32_t row, bool with_path) const {
    const auto& e = entries[row];
    return { with_path ? full_path(row) : std::string{}, e.size, e.modified, e.created, e.accessed, e.attributes };
}
std::vector<uint32_t> Volume::find_literals(const Query& query, const std::vector<std::string_view>& literals,
    uint64_t stop_after, const std::function<bool()>& cancelled, bool ordered) const {
    uint32_t checked = 0;
    auto check_cancelled = [&] {
        if (!(checked++ & 1023) && cancelled()) throw Error("Search cancelled or timed out");
    };
    auto children = [&](uint64_t id) {
        auto found = first_children_.find(id);
        return found == first_children_.end() ? no_row : found->second;
    };
    std::vector<uint32_t> seeds;
    bool entire_volume = false;
    const auto folded_root = fold(root);
    for (auto literal : literals) {
        check_cancelled();
        if (folded_root.find(literal) != folded_root.npos) { entire_volume = true; break; }
        bool trailing = literal.ends_with('\\');
        size_t end = literal.size() - size_t(trailing);
        auto slash = literal.rfind('\\', end - 1);
        auto begin = slash == literal.npos ? 0 : slash + 1;
        auto anchor = literal.substr(begin, end - begin);
        bool suffix = trailing && begin == 0;
        auto visit = [&](uint32_t uid) {
            check_cancelled();
            const auto& name = names[uid];
            if (suffix ? !name.lower.ends_with(anchor) : trailing ? name.lower != anchor : !name.lower.starts_with(anchor)) return;
            for (auto row = name.first; row != no_row; row = entries[row].next_name) {
                check_cancelled();
                const auto& entry = entries[row];
                if ((entry.attributes & deleted_attribute) || entry.id == root_id || (trailing && !(entry.attributes & FILE_ATTRIBUTE_DIRECTORY))) continue;
                auto path = full_path(row); if (path.empty()) continue;
                path = fold(path);
                if (trailing) path += '\\';
                else {
                    // The last fragment starts at a filename boundary. Check
                    // the preceding components against this occurrence only.
                    auto at = path.find_last_of('\\');
                    if (at == path.npos) continue;
                    path.resize(at + 1 + anchor.size());
                }
                if (path.ends_with(literal)) seeds.push_back(row);
            }
        };
        if (suffix) {
            const auto mask = name_bloom(anchor);
            for (uint32_t uid = 0; uid < names.size(); ++uid) {
                check_cancelled();
                if ((name_masks_[uid] & mask) == mask) visit(uid);
            }
        } else {
            auto first = std::lower_bound(sorted_names.begin(), sorted_names.end(), anchor,
                [&](uint32_t uid, std::string_view value) { return names[uid].lower < value; });
            for (auto it = first; it != sorted_names.end() && names[*it].lower.starts_with(anchor); ++it) visit(*it);
            // New names do not enter the lexical permutation until compaction.
            for (size_t uid = sorted_name_count_; uid < names.size(); ++uid) visit(static_cast<uint32_t>(uid));
        }
    }
    if (entire_volume) {
        seeds.clear();
        if (auto found = directory_rows.find(root_id); found != directory_rows.end()) seeds.push_back(found->second);
    }
    std::vector<uint32_t> hits;
    if (seeds.empty()) return hits;
    // Overlapping OR branches and repeated occurrences can select nested
    // roots. Visit each row once, preserving distinct NTFS hardlink names.
    std::vector<uint8_t> visited(entries.size());
    while (!seeds.empty()) {
        auto row = seeds.back(); seeds.pop_back(); check_cancelled();
        if (visited[row]) continue;
        visited[row] = 1;
        const auto& entry = entries[row];
        if (entry.attributes & deleted_attribute) continue;
        auto value = result(row); if (value.path.empty()) continue;
        if (entry.attributes & FILE_ATTRIBUTE_DIRECTORY) {
            for (auto child = children(entry.id); child != no_row; child = next_sibling_[child]) {
                check_cancelled();
                if (!visited[child]) seeds.push_back(child);
            }
        }
        if (entry.id == root_id) continue;
        const auto& name = names[entry.name];
        if (!query.may_match_name(name_masks_[entry.name]) || query.evaluate(name.text, name.lower, &value) != Truth::Yes) continue;
        hits.push_back(row);
        if (hits.size() >= stop_after && !ordered) return hits;
    }
    if (ordered) {
        std::stable_sort(hits.begin(), hits.end(), [&](uint32_t a, uint32_t b) {
            check_cancelled();
            return names[entries[a].name].lower < names[entries[b].name].lower;
        });
        if (hits.size() > stop_after) hits.resize(static_cast<size_t>(stop_after));
    }
    return hits;
}
std::vector<uint32_t> Volume::find(const Query& query, uint64_t stop_after, const std::function<bool()>& cancelled, bool ordered) const {
    std::vector<uint32_t> hits;
    if (!stop_after) return hits;
    if (!query.path_prefixes().empty()) {
        uint32_t checked = 0;
        auto check_cancelled = [&] {
            if (!(checked++ & 1023) && cancelled()) throw Error("Search cancelled or timed out");
        };
        auto children = [&](uint64_t id) {
            auto found = first_children_.find(id);
            return found == first_children_.end() ? no_row : found->second;
        };
        auto folded_root = fold(root);
        if (!folded_root.ends_with('\\')) folded_root += '\\';
        std::vector<uint64_t> roots;
        for (const auto& prefix : query.path_prefixes()) {
            check_cancelled();
            if (!prefix.starts_with(folded_root)) continue;
            std::vector<uint64_t> parents{root_id};
            size_t begin = folded_root.size();
            while (begin < prefix.size() && !parents.empty()) {
                auto end = prefix.find('\\', begin);
                auto component = std::string_view(prefix).substr(begin, end - begin);
                std::vector<uint64_t> next;
                for (auto id : parents) for (auto row = children(id); row != no_row; row = next_sibling_[row]) {
                    check_cancelled();
                    const auto& entry = entries[row];
                    if ((entry.attributes & FILE_ATTRIBUTE_DIRECTORY) && !(entry.attributes & deleted_attribute) && names[entry.name].lower == component)
                        next.push_back(entry.id);
                }
                parents = std::move(next); begin = end + 1;
            }
            roots.insert(roots.end(), parents.begin(), parents.end());
        }
        // Resolve and walk the bounded subtrees only. Keep the original query
        // as the final predicate: wildcards still cross separators, and case,
        // metadata, OR/AND and negation retain their existing semantics.
        std::vector<uint32_t> pending;
        for (auto id : roots) if (auto row = children(id); row != no_row) pending.push_back(row);
        const bool needs_path = query.requires_path();
        while (!pending.empty()) {
            auto row = pending.back(); pending.pop_back();
            for (; row != no_row; row = next_sibling_[row]) {
                check_cancelled();
                const auto& entry = entries[row];
                if ((entry.attributes & deleted_attribute) || entry.id == root_id) continue;
                auto value = result(row, needs_path);
                if (needs_path && value.path.empty()) continue;
                if (entry.attributes & FILE_ATTRIBUTE_DIRECTORY)
                    if (auto child = children(entry.id); child != no_row) pending.push_back(child);
                const auto& name = names[entry.name];
                if (query.may_match_name(name_masks_[entry.name]) && query.evaluate(name.text, name.lower, &value) == Truth::Yes) {
                    hits.push_back(row);
                    if (hits.size() >= stop_after && !ordered) return hits;
                }
            }
        }
        if (ordered) {
            std::stable_sort(hits.begin(), hits.end(), [&](uint32_t a, uint32_t b) {
                check_cancelled();
                return names[entries[a].name].lower < names[entries[b].name].lower;
            });
            if (hits.size() > stop_after) hits.resize(static_cast<size_t>(stop_after));
        }
        return hits;
    }
    if (auto literals = query.path_literals(); !literals.empty()) return find_literals(query, literals, stop_after, cancelled, ordered);
    auto prefix = query.prefix();
    auto first = prefix.empty() ? sorted_names.begin() : std::lower_bound(sorted_names.begin(), sorted_names.end(), prefix,
        [&](uint32_t uid, const std::string& value) { return names[uid].lower < value; });
    uint32_t checked = 0;
    const auto* simple_path = query.simple_path_literal();
    std::string_view path_needle = simple_path ? (simple_path->match_case ? simple_path->value : simple_path->folded) : std::string_view{};
    std::vector<uint8_t> name_matches;
    if (simple_path) {
        name_matches.resize(names.size()); auto bloom = name_bloom(simple_path->folded);
        for (uint32_t uid = 0; uid < names.size(); ++uid) if ((name_masks_[uid] & bloom) == bloom)
            name_matches[uid] = (simple_path->match_case ? names[uid].text : names[uid].lower).find(path_needle) != std::string::npos;
    }
    // Memoize reachability and ancestor matches in compact row-indexed bytes.
    // Plain path substrings without separators match an individual component;
    // no full path needs to be allocated for the millions of nonmatching files.
    std::vector<uint8_t> ancestors(entries.size());
    bool root_match = simple_path && (simple_path->match_case ? root : fold(root)).find(path_needle) != std::string::npos;
    auto ancestry = [&](uint32_t row) {
        std::array<uint32_t, 512> trail; size_t count = 0; uint8_t flags = 1;
        while (row < entries.size()) {
            if (ancestors[row]) { flags = ancestors[row]; break; }
            if (entries[row].attributes & deleted_attribute) break;
            if (entries[row].id == root_id) { flags = root_match ? 7 : 3; break; }
            if (count == trail.size()) break;
            trail[count++] = row; row = parent_row(row);
        }
        while (count) { auto item = trail[--count]; if (simple_path && (flags & 2) && name_matches[entries[item].name]) flags |= 4; ancestors[item] = flags; }
        return flags;
    };
    if (simple_path && !ordered) {
        for (uint32_t row = 0; row < entries.size(); ++row) {
            if (!(row & 16383) && cancelled()) throw Error("Search cancelled or timed out");
            const auto& entry = entries[row]; if ((entry.attributes & deleted_attribute) || entry.id == root_id) continue;
            auto flags = ancestry(parent_row(row));
            if ((flags & 2) && ((flags & 4) || name_matches[entry.name])) { hits.push_back(row); if (hits.size() >= stop_after) break; }
        }
        return hits;
    }
    auto visit = [&](uint32_t uid) {
        if (!(checked++ & 1023) && cancelled()) throw Error("Search cancelled or timed out");
        if (!query.may_match_name(name_masks_[uid])) return false;
        const auto& name = names[uid];
        auto name_match = query.evaluate(name.text, name.lower);
        if (name_match == Truth::No) return false;
        for (auto row = name.first; row != no_row; row = entries[row].next_name) {
            if (!(checked++ & 4095) && cancelled()) throw Error("Search cancelled or timed out");
            const auto& entry = entries[row];
            if ((entry.attributes & deleted_attribute) || entry.id == root_id) continue;
            auto flags = ancestry(parent_row(row)); if (!(flags & 2)) continue;
            if (simple_path) {
                if (!(flags & 4) && !name_matches[entry.name]) continue;
            } else if (name_match == Truth::Unknown) {
                auto value = result(row, query.requires_path());
                if (query.requires_path() && value.path.empty()) continue;
                if (query.evaluate(name.text, name.lower, &value) != Truth::Yes) continue;
            }
            hits.push_back(row);
            if (hits.size() >= stop_after) return true;
        }
        return false;
    };
    if (prefix.empty() && !ordered) {
        // Scan names in allocation order: traversing the lexical permutation causes
        // a cache miss for almost every unique name on a multi-million-file index.
        for (uint32_t uid = 0; uid < names.size(); ++uid) if (visit(uid)) return hits;
    } else {
        for (auto i = first; i != sorted_names.end(); ++i) {
            if (!prefix.empty() && !names[*i].lower.starts_with(prefix)) break;
            if (visit(*i)) return hits;
        }
        auto delta_start = hits.size();
        for (auto uid = sorted_name_count_; uid < names.size(); ++uid) if (visit(static_cast<uint32_t>(uid))) return hits;
        if (ordered && hits.size() > delta_start) {
            auto less = [&](uint32_t a, uint32_t b) { return names[entries[a].name].lower < names[entries[b].name].lower; };
            std::stable_sort(hits.begin() + delta_start, hits.end(), less);
            std::inplace_merge(hits.begin(), hits.begin() + delta_start, hits.end(), less);
        }
    }
    return hits;
}
void Engine::set_volumes(std::vector<std::shared_ptr<Volume>> value) { std::unique_lock lock(mutex); volumes = std::move(value); ++topology_; }
Reply Engine::status() const {
    std::shared_lock lock(mutex); Reply reply; std::ostringstream text;
    for (auto& volume : volumes) {
        std::shared_lock volume_lock(volume->mutex);
        text << volume->root << '\t' << volume->state << "\tfiles=" << volume->files << "\tdirectories=" << volume->directories
            << "\tpending=" << volume->pending_updates << "\n";
        reply.total += volume->files + volume->directories;
    }
    reply.message = text.str(); return reply;
}
Reply Engine::search(const Request& request, const std::function<bool()>& cancelled) {
    auto start = std::chrono::steady_clock::now();
    Query query(request); Reply reply;
    std::shared_lock lock(mutex);
    std::vector<std::shared_lock<std::shared_mutex>> locks;
    uint64_t generation = topology_;
    bool metadata = query.requires_metadata() || request.sort == Sort::Size || request.sort == Sort::Modified || request.sort == Sort::Created;
    bool any_ready = false;
    for (auto& volume : volumes) {
        locks.emplace_back(volume->mutex); generation += metadata ? volume->generation : volume->name_generation;
        any_ready |= volume->ready;
        if (!volume->ready) reply.message += volume->root + " " + volume->state + "; ";
    }
    if (!any_ready) throw Error(reply.message.empty() ? "No indexed NTFS volumes are available" : reply.message);
    if (cancelled()) throw Error("Search cancelled or timed out");
    auto limit = request.limit;
    bool early = request.sort == Sort::None && !(request.flags & EverythingSyntax) && !(request.flags & (Existing | NonExisting));
    std::shared_ptr<Cached> cache;
    auto same = [&](const Request& a) { return a.flags == request.flags && a.patterns == request.patterns && a.sort == request.sort && a.descending == request.descending; };
    if (!early) { std::lock_guard guard(cache_mutex_); if (cache_ && cache_->generation == generation && same(cache_->request)) cache = cache_; }
    if (!cache) {
        cache = std::make_shared<Cached>(); cache->generation = generation; cache->request = request; cache->volumes = volumes;
        std::vector<size_t> boundaries;
        for (uint32_t v = 0; v < volumes.size(); ++v) {
            auto remaining = early ? limit - std::min<uint64_t>(limit, cache->rows.size()) : UINT64_MAX;
            boundaries.push_back(cache->rows.size());
            if (!volumes[v]->ready) continue;
            auto rows = volumes[v]->find(query, remaining, cancelled, request.sort == Sort::Name);
            for (auto row : rows) cache->rows.push_back({v, row});
            if (early && cache->rows.size() >= limit) break;
        }
        auto compare = [&](RowRef a, RowRef b) {
            const auto& va = *volumes[a.volume]; const auto& vb = *volumes[b.volume];
            const auto& ea = va.entries[a.row]; const auto& eb = vb.entries[b.row];
            switch (request.sort) {
            case Sort::Size: return ea.size < eb.size;
            case Sort::Modified: return ea.modified < eb.modified;
            case Sort::Created: return ea.created < eb.created;
            case Sort::Extension: {
                auto& an = va.names[ea.name].lower; auto& bn = vb.names[eb.name].lower;
                auto ap = an.find_last_of('.'), bp = bn.find_last_of('.');
                return (ap == an.npos ? std::string_view{} : std::string_view(an).substr(ap)) < (bp == bn.npos ? std::string_view{} : std::string_view(bn).substr(bp));
            }
            default: return va.names[ea.name].lower < vb.names[eb.name].lower;
            }
        };
        if (request.sort == Sort::Name) {
            for (size_t v = 1; v < boundaries.size(); ++v) std::inplace_merge(cache->rows.begin(), cache->rows.begin() + boundaries[v],
                cache->rows.begin() + (v + 1 < boundaries.size() ? boundaries[v + 1] : cache->rows.size()), compare);
        } else if (request.sort == Sort::Path) {
            // Store each parent path once, instead of allocating and case-folding
            // complete paths during every comparison (millions of allocations).
            std::vector<std::unordered_map<uint64_t, std::string>> parents(volumes.size());
            struct PathKey { RowRef ref; std::string_view parent, name; };
            std::vector<PathKey> keys; keys.reserve(cache->rows.size());
            for (auto ref : cache->rows) {
                if (!(keys.size() & 4095) && cancelled()) throw Error("Search cancelled or timed out");
                const auto& volume = *volumes[ref.volume]; const auto& entry = volume.entries[ref.row];
                auto [it, added] = parents[ref.volume].try_emplace(entry.parent);
                if (added) { auto path = volume.full_path(ref.row); it->second = fold(path.substr(0, path.find_last_of('\\') + 1)); }
                keys.push_back({ref, it->second, volume.names[entry.name].lower});
            }
            uint64_t comparisons = 0;
            std::stable_sort(keys.begin(), keys.end(), [&](const PathKey& a, const PathKey& b) {
                if (!(comparisons++ & 65535) && cancelled()) throw Error("Search cancelled or timed out");
                std::string_view left[]{a.parent, a.name}, right[]{b.parent, b.name};
                size_t l = 0, r = 0;
                while (l < 2 && r < 2) {
                    auto length = std::min(left[l].size(), right[r].size());
                    auto order = left[l].substr(0, length).compare(right[r].substr(0, length));
                    if (order) return order < 0;
                    left[l].remove_prefix(length); right[r].remove_prefix(length);
                    if (left[l].empty()) ++l; if (right[r].empty()) ++r;
                }
                return l == 2 && r != 2;
            });
            for (size_t i = 0; i < keys.size(); ++i) cache->rows[i] = keys[i].ref;
        } else if (request.sort != Sort::None) {
            uint64_t comparisons = 0;
            std::stable_sort(cache->rows.begin(), cache->rows.end(), [&](RowRef a, RowRef b) {
                if (!(comparisons++ & 65535) && cancelled()) throw Error("Search cancelled or timed out");
                return compare(a, b);
            });
        }
        if (request.descending) std::reverse(cache->rows.begin(), cache->rows.end());
        if (!early && !(request.flags & (Existing | NonExisting))) { std::lock_guard guard(cache_mutex_); cache_ = cache; }
    }
    uint64_t accepted = 0;
    if (!(request.flags & (Existing | NonExisting))) {
        accepted = cache->rows.size();
        auto end = request.offset + std::min(limit, accepted - std::min(accepted, request.offset));
        if (!(request.flags & CountOnly)) for (auto position = request.offset; position < end; ++position) {
            if (!(position & 4095) && cancelled()) throw Error("Search cancelled or timed out");
            auto ref = cache->rows[static_cast<size_t>(position)];
            reply.results.push_back(volumes[ref.volume]->result(ref.row));
        }
    } else for (auto ref : cache->rows) {
        if (!(accepted & 4095) && cancelled()) throw Error("Search cancelled or timed out");
        Result value;
        if (request.flags & (Existing | NonExisting)) {
            value = volumes[ref.volume]->result(ref.row);
            bool exists = !value.path.empty() && GetFileAttributesW(wide(value.path).c_str()) != INVALID_FILE_ATTRIBUTES;
            if ((request.flags & Existing) ? !exists : exists) continue;
        }
        auto position = accepted++;
        if (position < request.offset || position - request.offset >= limit || (request.flags & CountOnly)) continue;
        if (value.path.empty()) value = volumes[ref.volume]->result(ref.row);
        if (!value.path.empty()) reply.results.push_back(std::move(value));
    }
    reply.total = (request.flags & EverythingSyntax) ? accepted : std::min<uint64_t>(accepted, request.limit);
    reply.generation = generation;
    reply.search_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    return reply;
}
}
