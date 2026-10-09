#include "query.hpp"
#include <charconv>
#include <cctype>
#include <sstream>

namespace wl {
namespace {
uint32_t codepoint(std::string_view text, size_t& at) {
    if (at >= text.size()) return 0;
    auto c = static_cast<unsigned char>(text[at++]);
    if (c < 128) return c;
    unsigned n = c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
    uint32_t value = c & ((1u << (6 - n)) - 1);
    while (n-- && at < text.size()) value = (value << 6) | (static_cast<unsigned char>(text[at++]) & 63);
    return value;
}
bool character_match(std::string_view pattern, size_t& p, uint32_t c) {
    if (pattern[p] == '?') { ++p; return true; }
    if (pattern[p] != '[') return codepoint(pattern, p) == c;
    size_t begin = p++, end = pattern.find(']', p + (p < pattern.size() && pattern[p] == ']' ? 1 : 0));
    if (end == pattern.npos) { p = begin + 1; return c == '['; }
    bool negate = p < end && (pattern[p] == '!' || pattern[p] == '^');
    if (negate) ++p;
    bool matched = false;
    while (p < end) {
        auto first = codepoint(pattern, p), last = first;
        if (p + 1 < end && pattern[p] == '-') { ++p; last = codepoint(pattern, p); }
        matched |= c >= first && c <= last;
    }
    p = end + 1;
    return matched != negate;
}
bool has_glob(std::string_view value) { return value.find_first_of("*?[") != value.npos; }
bool word(unsigned char c) { return c >= 128 || std::isalnum(c) || c == '_'; }
bool text_match(const Term& term, std::string_view original, std::string_view lower) {
    if (term.regex) return term.regex->matches(original);
    auto target = term.match_case ? original : lower;
    std::string_view needle = term.match_case ? term.value : term.folded;
    if (term.glob) return glob_match(needle, target);
    auto pos = target.find(needle);
    if (!term.whole_word) return pos != target.npos;
    while (pos != target.npos) {
        if ((pos == 0 || !word(static_cast<unsigned char>(target[pos - 1]))) &&
            (pos + needle.size() == target.size() || !word(static_cast<unsigned char>(target[pos + needle.size()])))) return true;
        pos = target.find(needle, pos + 1);
    }
    return false;
}
uint64_t size_value(std::string text) {
    size_t end = 0;
    double number;
    try { number = std::stod(text, &end); } catch (...) { throw Error("Invalid size: " + text); }
    auto unit = fold(text.substr(end));
    uint64_t multiplier = 1;
    if (unit == "k" || unit == "kb" || unit == "kib") multiplier = 1024;
    else if (unit == "m" || unit == "mb" || unit == "mib") multiplier = 1024ull * 1024;
    else if (unit == "g" || unit == "gb" || unit == "gib") multiplier = 1024ull * 1024 * 1024;
    else if (unit == "t" || unit == "tb" || unit == "tib") multiplier = 1024ull * 1024 * 1024 * 1024;
    else if (!unit.empty() && unit != "b") throw Error("Unknown size unit: " + unit);
    if (!std::isfinite(number) || number < 0 || number * multiplier >= double(UINT64_MAX)) throw Error("Size out of range");
    return static_cast<uint64_t>(number * multiplier);
}
uint64_t date_value(std::string text, bool end_of_day = false) {
    SYSTEMTIME local{}; GetLocalTime(&local); local.wHour = local.wMinute = local.wSecond = local.wMilliseconds = 0;
    auto lower = fold(text);
    if (lower != "today" && lower != "yesterday") {
        int year = 0, month = 0, day = 0;
        bool format = text.size() == 10 && text[4] == '-' && text[7] == '-';
        for (size_t i = 0; i < text.size(); ++i) if (i != 4 && i != 7 && (text[i] < '0' || text[i] > '9')) format = false;
        if (!format || sscanf_s(text.c_str(), "%d-%d-%d", &year, &month, &day) != 3 || year < 1601 || year > 9999 || month < 1 || month > 12 || day < 1 || day > 31)
            throw Error("Date must be YYYY-MM-DD, today, or yesterday");
        local.wYear = static_cast<WORD>(year); local.wMonth = static_cast<WORD>(month); local.wDay = static_cast<WORD>(day);
    }
    // Advance the calendar day before timezone conversion so 23/25-hour DST
    // days have the correct boundaries too.
    FILETIME calendar{}; SYSTEMTIME verified{};
    if (!SystemTimeToFileTime(&local, &calendar) || !FileTimeToSystemTime(&calendar, &verified) ||
        verified.wYear != local.wYear || verified.wMonth != local.wMonth || verified.wDay != local.wDay) throw Error("Invalid date: " + text);
    auto calendar_value = filetime_value(calendar);
    if (lower == "yesterday") calendar_value -= 864000000000ull;
    if (end_of_day) calendar_value += 864000000000ull;
    calendar = FILETIME{DWORD(calendar_value), DWORD(calendar_value >> 32)};
    if (!FileTimeToSystemTime(&calendar, &local)) throw Error("Date is out of range");
    SYSTEMTIME utc{}; FILETIME time{};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) || !SystemTimeToFileTime(&utc, &time)) throw Error("Invalid date: " + text);
    auto value = filetime_value(time);
    return end_of_day ? value - 1 : value;
}
void range(Term& term, std::string value, bool date) {
    auto number = [&](std::string part) { return date ? date_value(std::move(part)) : size_value(std::move(part)); };
    auto upper = [&](std::string part) { return date ? date_value(std::move(part), true) : size_value(std::move(part)); };
    auto split = value.find("..");
    if (split != value.npos) {
        term.minimum = number(value.substr(0, split)); term.maximum = upper(value.substr(split + 2));
    } else if (value.starts_with(">=")) term.minimum = number(value.substr(2));
    else if (value.starts_with("<=")) term.maximum = upper(value.substr(2));
    else if (value.starts_with('>')) { auto n = upper(value.substr(1)); if (n == UINT64_MAX) throw Error("Range overflow"); term.minimum = n + 1; }
    else if (value.starts_with('<')) { auto n = number(value.substr(1)); if (!n) throw Error("Range is empty"); term.maximum = n - 1; }
    else { term.minimum = number(value); term.maximum = upper(value); }
    if (term.minimum > term.maximum) throw Error("Range is empty");
}
enum class TokenKind { Word, Open, Close, Or, Not };
struct Token { TokenKind kind; std::string text; };
std::vector<Token> tokenize(std::string_view value) {
    std::vector<Token> result;
    std::string token; bool quoted = false, started = false; unsigned depth = 0;
    auto flush = [&] { if (started) { result.push_back({TokenKind::Word, std::move(token)}); token.clear(); started = false; } };
    for (size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        if (c == '"') { quoted = !quoted; started = true; continue; }
        bool comparator = false;
        if (!token.empty() && token.back() == ':') {
            auto key = fold(token); key.pop_back(); auto colon = key.find_last_of(':'); if (colon != key.npos) key.erase(0, colon + 1);
            comparator = key == "size" || key == "dm" || key == "dc" || key == "da" || key.starts_with("date");
        }
        if (!quoted && std::isspace(static_cast<unsigned char>(c))) flush();
        else if (!quoted && c == '|') { flush(); result.push_back({TokenKind::Or, {}}); }
        else if (!quoted && c == '!' && !started) result.push_back({TokenKind::Not, {}});
        else if (!quoted && c == '<' && !comparator) { flush(); result.push_back({TokenKind::Open, {}}); ++depth; }
        else if (!quoted && c == '>' && !comparator && (depth || !started)) { flush(); result.push_back({TokenKind::Close, {}}); if (depth) --depth; }
        else { token += c; started = true; }
    }
    if (quoted) throw Error("Unclosed quote");
    flush();
    return result;
}
Term make_term(std::string value, uint32_t flags, bool syntax) {
    Term term; term.match_case = (flags & MatchCase) != 0; term.whole_word = (flags & WholeWord) != 0;
    bool use_regex = (flags & Regex) != 0;
    bool explicit_field = false;
    if (syntax) {
        for (;;) {
            auto colon = value.find(':');
            if (colon == value.npos || colon == 1) break;
            auto key = fold(value.substr(0, colon));
            if (key == "case") term.match_case = true;
            else if (key == "nocase") term.match_case = false;
            else if (key == "regex") use_regex = true;
            else if (key == "wholeword" || key == "ww") term.whole_word = true;
            else if (key == "path") { term.field = Field::Path; explicit_field = true; }
            else if (key == "parent") { term.field = Field::Parent; explicit_field = true; }
            else if (key == "file" || key == "files") { term.field = Field::File; explicit_field = true; }
            else if (key == "folder" || key == "folders") { term.field = Field::Folder; explicit_field = true; }
            else if (key == "ext") { term.field = Field::Extension; explicit_field = true; }
            else if (key == "size") { term.field = Field::Size; explicit_field = true; }
            else if (key == "dm" || key == "datemodified" || key == "date-modified") { term.field = Field::Modified; explicit_field = true; }
            else if (key == "dc" || key == "datecreated" || key == "date-created") { term.field = Field::Created; explicit_field = true; }
            else if (key == "da" || key == "dateaccessed") { term.field = Field::Accessed; explicit_field = true; }
            else if (key == "attrib" || key == "attributes") { term.field = Field::Attributes; explicit_field = true; }
            else throw Error("Unsupported search function: " + key);
            value.erase(0, colon + 1);
            if (explicit_field || key == "regex") break;
        }
    }
    if (!explicit_field) {
        bool path = (flags & MatchPath) != 0;
        if (!(flags & Basename)) {
            path |= !syntax && !has_glob(value) && !use_regex;
            path |= value.find_first_of("/\\") != value.npos && !use_regex;
        }
        term.field = path ? Field::Path : Field::Name;
    }
    if (term.field == Field::Size) range(term, value, false);
    if (term.field == Field::Modified || term.field == Field::Created || term.field == Field::Accessed) range(term, value, true);
    if (!use_regex) value = normalize_path(std::move(value));
    term.value = std::move(value); term.folded = fold(term.value);
    term.glob = !use_regex && has_glob(term.value);
    if (use_regex) {
        term.regex = std::make_shared<Pattern>(term.value, term.match_case);
    }
    return term;
}
using Expression = std::vector<std::vector<Term>>;
Expression conjunction(const Expression& left, const Expression& right) {
    if (left.size() * right.size() > 256) throw Error("Search grouping is too complex");
    Expression result;
    for (auto& a : left) for (auto& b : right) {
        if (a.size() + b.size() > 128) throw Error("Too many search terms");
        auto group = a; group.insert(group.end(), b.begin(), b.end()); result.push_back(std::move(group));
    }
    return result;
}
Expression negate(Expression input) {
    Expression output{{}};
    for (auto& group : input) {
        Expression alternatives;
        for (auto term : group) { term.negate = !term.negate; alternatives.push_back({std::move(term)}); }
        output = conjunction(output, alternatives);
    }
    return output;
}
class ExpressionParser {
    std::vector<Token> tokens_;
    size_t position_ = 0;
    uint32_t flags_;
    unsigned depth_ = 0;
    Expression atom() {
        if (position_ >= tokens_.size()) throw Error("Missing search expression");
        bool negative = false;
        while (position_ < tokens_.size() && tokens_[position_].kind == TokenKind::Not) { negative = !negative; ++position_; }
        if (position_ == tokens_.size()) throw Error("Missing expression after !");
        Expression result;
        if (tokens_[position_].kind == TokenKind::Open) {
            if (++depth_ > 32) throw Error("Search groups are nested too deeply"); ++position_; result = disjunction();
            if (position_ == tokens_.size() || tokens_[position_].kind != TokenKind::Close) throw Error("Unclosed search group"); ++position_; --depth_;
        } else {
            if (tokens_[position_].kind != TokenKind::Word) throw Error("Missing search term");
            result = {{make_term(tokens_[position_++].text, flags_, true)}};
        }
        return negative ? negate(std::move(result)) : result;
    }
    Expression and_terms() {
        auto result = atom();
        while (position_ < tokens_.size() && tokens_[position_].kind != TokenKind::Or && tokens_[position_].kind != TokenKind::Close) result = conjunction(result, atom());
        return result;
    }
    Expression disjunction() {
        auto result = and_terms();
        while (position_ < tokens_.size() && tokens_[position_].kind == TokenKind::Or) {
            ++position_; auto other = and_terms(); result.insert(result.end(), other.begin(), other.end());
            if (result.size() > 256) throw Error("Too many OR branches");
        }
        return result;
    }
public:
    ExpressionParser(std::string text, uint32_t flags) : tokens_(tokenize(text)), flags_(flags) { if (tokens_.size() > 2048) throw Error("Too many search tokens"); }
    Expression parse() {
        if (tokens_.empty()) return {{}};
        auto result = disjunction(); if (position_ != tokens_.size()) throw Error("Unexpected closing group"); return result;
    }
};
}
bool glob_match(std::string_view pattern, std::string_view value) {
    size_t p = 0, v = 0, star = pattern.npos, retry = 0;
    while (v < value.size()) {
        if (p < pattern.size() && pattern[p] == '*') { star = ++p; retry = v; continue; }
        size_t next_v = v, next_p = p; auto c = codepoint(value, next_v);
        if (p < pattern.size() && character_match(pattern, next_p, c)) { p = next_p; v = next_v; continue; }
        if (star == pattern.npos) return false;
        p = star; codepoint(value, retry); v = retry;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}
Truth Term::evaluate(std::string_view name, std::string_view lower_name, const Result* result) const {
    bool match = false;
    if (field == Field::Name) match = text_match(*this, name, lower_name);
    else if (field == Field::Extension) {
        auto dot = lower_name.find_last_of('.'); auto ext = dot == lower_name.npos ? std::string_view{} : lower_name.substr(dot + 1);
        size_t at = 0;
        do { auto end = folded.find(';', at); if (end == folded.npos) end = folded.size(); auto part = std::string_view(folded).substr(at, end - at); if (part.starts_with('.')) part.remove_prefix(1); match |= glob_match(part, ext); at = end + 1; } while (at <= folded.size());
    } else if (!result) return Truth::Unknown;
    else if (field == Field::File || field == Field::Folder) {
        match = ((result->attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == (field == Field::Folder);
        if (match && !value.empty()) match = text_match(*this, name, lower_name);
    } else if (field == Field::Size || field == Field::Modified || field == Field::Created || field == Field::Accessed) {
        uint64_t actual = field == Field::Size ? result->size : field == Field::Modified ? result->modified : field == Field::Created ? result->created : result->accessed;
        match = actual >= minimum && actual <= maximum;
        if (field == Field::Size && (result->attributes & FILE_ATTRIBUTE_DIRECTORY)) match = false;
    } else if (field == Field::Attributes) {
        match = true;
        for (char c : folded) {
            uint32_t bit = c == 'h' ? FILE_ATTRIBUTE_HIDDEN : c == 's' ? FILE_ATTRIBUTE_SYSTEM : c == 'r' ? FILE_ATTRIBUTE_READONLY : c == 'a' ? FILE_ATTRIBUTE_ARCHIVE : c == 'd' ? FILE_ATTRIBUTE_DIRECTORY : c == 'l' ? FILE_ATTRIBUTE_REPARSE_POINT : 0;
            match &= bit && (result->attributes & bit);
        }
    } else {
        auto path = field == Field::Parent ? directory(result->path) : result->path;
        if (regex) std::replace(path.begin(), path.end(), '\\', '/');
        match = text_match(*this, path, match_case ? path : fold(path));
    }
    return match != negate ? Truth::Yes : Truth::No;
}
Query::Query(const Request& request) {
    if (request.patterns.size() > 256) throw Error("Too many patterns");
    if (request.flags & EverythingSyntax) {
        groups_ = {{}};
        for (size_t i = 0; i < request.patterns.size(); ++i) {
            auto expression = (request.flags & Regex) && !i ? Expression{{make_term(request.patterns[i], request.flags, false)}}
                : ExpressionParser(request.patterns[i], request.flags & ~Regex).parse();
            groups_ = conjunction(groups_, expression);
        }
    } else if (request.flags & AllPatterns) {
        groups_.emplace_back(); for (auto& pattern : request.patterns) groups_.back().push_back(make_term(pattern, request.flags, false));
    } else for (auto& pattern : request.patterns) groups_.push_back({ make_term(pattern, request.flags, false) });
    if (groups_.empty()) groups_.emplace_back();
    for (auto& group : groups_) {
        uint64_t required = 0;
        for (auto& term : group) {
            if (term.negate || term.regex || (term.field != Field::Name && term.field != Field::File && term.field != Field::Folder)) continue;
            if (!term.glob) { required |= name_bloom(term.folded); continue; }
            if (term.folded.find('[') != term.folded.npos) continue;
            size_t start = 0;
            for (size_t i = 0; i < term.folded.size(); ++i) if (term.folded[i] == '*' || term.folded[i] == '?' || term.folded[i] == '[') {
                required |= name_bloom(std::string_view(term.folded).substr(start, i - start));
                if (term.folded[i] == '[') { auto end = term.folded.find(']', i + 1); if (end != term.folded.npos) i = end; }
                start = i + 1;
            }
            required |= name_bloom(std::string_view(term.folded).substr(start));
        }
        masks_.push_back(required);
    }
    for (const auto& group : groups_) {
        std::string prefix;
        for (const auto& term : group) {
            if (term.field != Field::Path || term.negate || term.regex || !term.glob) continue;
            const auto& value = term.folded;
            if (value.size() < 3 || value[0] < 'a' || value[0] > 'z' || value[1] != ':' || value[2] != '\\') continue;
            auto literal = value.substr(0, value.find_first_of("*?["));
            auto separator = literal.find_last_of('\\');
            if (separator != literal.npos && separator + 1 > prefix.size()) prefix = literal.substr(0, separator + 1);
        }
        // A positive bound is required in EVERY disjunct. Negative terms and
        // unanchored substring/regex matches cannot safely narrow a subtree.
        if (prefix.empty()) { path_prefixes_.clear(); return; }
        path_prefixes_.push_back(std::move(prefix));
    }
    std::sort(path_prefixes_.begin(), path_prefixes_.end());
    size_t kept = 0;
    for (size_t i = 0; i < path_prefixes_.size(); ++i) {
        if (kept && path_prefixes_[i].starts_with(path_prefixes_[kept - 1])) continue;
        if (kept != i) path_prefixes_[kept] = std::move(path_prefixes_[i]);
        ++kept;
    }
    path_prefixes_.resize(kept);
}
Truth Query::evaluate(std::string_view name, std::string_view lower, const Result* result) const {
    bool unknown = false;
    for (const auto& group : groups_) {
        Truth state = Truth::Yes;
        for (const auto& term : group) { auto value = term.evaluate(name, lower, result); if (value == Truth::No) { state = Truth::No; break; } if (value == Truth::Unknown) state = value; }
        if (state == Truth::Yes) return state;
        unknown |= state == Truth::Unknown;
    }
    return unknown ? Truth::Unknown : Truth::No;
}
std::string Query::prefix() const {
    std::string prefix;
    if (groups_.size() != 1) return prefix;
    for (auto& term : groups_[0]) if (term.field == Field::Name && term.glob && !term.negate) {
        auto candidate = term.folded.substr(0, term.folded.find_first_of("*?["));
        if (candidate.size() > prefix.size()) prefix = std::move(candidate);
    }
    return prefix;
}
bool Query::requires_path() const {
    for (auto& group : groups_) for (auto& term : group) if (term.field == Field::Path || term.field == Field::Parent) return true;
    return false;
}
std::vector<std::string_view> Query::path_literals() const {
    std::vector<std::string_view> literals;
    for (const auto& group : groups_) {
        std::string_view best;
        size_t best_score = 0;
        for (const auto& term : group) {
            if (term.field != Field::Path || term.negate || term.glob || term.regex) continue;
            std::string_view text = term.folded;
            if (text.find('\\') == text.npos) continue;
            bool trailing = text.ends_with('\\');
            auto end = text.size() - size_t(trailing);
            if (!end) continue;
            auto slash = text.rfind('\\', end - 1);
            auto begin = slash == text.npos ? 0 : slash + 1;
            if (begin == end) continue;
            // Prefer a longer indexed final component. A single component
            // followed by a slash needs a suffix scan rather than a prefix.
            size_t score = (end - begin) * 2 + size_t(begin != 0);
            if (best.empty() || score > best_score) { best = text; best_score = score; }
        }
        if (best.empty()) return {};
        if (std::find(literals.begin(), literals.end(), best) == literals.end()) literals.push_back(best);
    }
    return literals;
}
bool Query::requires_metadata() const {
    for (auto& group : groups_) for (auto& term : group) if (term.field >= Field::Size) return true;
    return false;
}
const Term* Query::simple_path_literal() const {
    if (groups_.size() != 1 || groups_[0].size() != 1) return nullptr;
    const auto& term = groups_[0][0];
    return term.field == Field::Path && !term.negate && !term.glob && !term.regex && !term.whole_word && term.value.find_first_of("/\\") == term.value.npos ? &term : nullptr;
}
}
