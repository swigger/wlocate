#pragma once
#include "common.hpp"
#include "regex.hpp"

namespace wl {
bool glob_match(std::string_view pattern, std::string_view value);
enum class Truth { No, Yes, Unknown };
enum class Field { Name, Path, Parent, Extension, File, Folder, Size, Modified, Created, Accessed, Attributes };
struct Term {
    Field field = Field::Name;
    std::string value, folded;
    bool negate = false, match_case = false, glob = false, whole_word = false;
    uint64_t minimum = 0, maximum = UINT64_MAX;
    std::shared_ptr<Pattern> regex;
    Truth evaluate(std::string_view name, std::string_view lower_name, const Result* result) const;
};
class Query {
public:
    explicit Query(const Request& request);
    Truth evaluate(std::string_view name, std::string_view lower, const Result* result = nullptr) const;
    std::string prefix() const;
    bool requires_path() const;
    bool requires_metadata() const;
    const Term* simple_path_literal() const;
    // Folded, non-overlapping absolute directory prefixes covering every OR
    // branch. Empty means the query still needs a general name/path scan.
    const std::vector<std::string>& path_prefixes() const { return path_prefixes_; }
    // One necessary literal path fragment per OR branch, or empty if any
    // branch cannot be narrowed safely. Views remain owned by this query.
    std::vector<std::string_view> path_literals() const;
    bool may_match_name(uint64_t bloom) const {
        for (auto required : masks_) if ((bloom & required) == required) return true;
        return false;
    }
private:
    std::vector<std::vector<Term>> groups_;
    std::vector<uint64_t> masks_;
    std::vector<std::string> path_prefixes_;
};
}
