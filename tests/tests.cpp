#include "ntfs.hpp"
#include <iostream>
#include <cstring>
#include <fstream>

namespace {
unsigned checks = 0;
void require(bool value, const char* message) { ++checks; if (!value) throw wl::Error(message); }
template<class F> void rejects(F fn, const char* message) { bool failed = false; try { fn(); } catch (...) { failed = true; } require(failed, message); }
template<class T> void put(std::vector<uint8_t>& bytes, size_t at, T value) { memcpy(bytes.data() + at, &value, sizeof(value)); }
void encoding_tests() {
    require(GetACP() == CP_UTF8, "embedded manifest selects UTF-8 process ACP");
    const std::string text = "中文-é-😀";
    wchar_t converted[64]{};
    auto length = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), converted, 64);
    require(length > 0 && std::wstring_view(converted, length) == L"中文-é-😀", "default Win32 code page decodes UTF-8");
    auto file = std::filesystem::temp_directory_path() / (L"wlocate-utf8-" + std::to_wstring(GetCurrentProcessId()) + L"-中文-é-😀.tmp");
    auto path = wl::utf8(file.wstring());
    wl::Handle created(CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
    require(created && std::filesystem::exists(file), "UTF-8 narrow filename reaches the same file as a UTF-16 Windows path");
}
std::vector<uint8_t> record_fixture(bool extension) {
    std::vector<uint8_t> bytes(1024); memcpy(bytes.data(), "FILE", 4);
    put<uint16_t>(bytes, 4, 48); put<uint16_t>(bytes, 6, 3); put<uint16_t>(bytes, 0x10, 7);
    put<uint16_t>(bytes, 0x14, 56); put<uint16_t>(bytes, 0x16, 1); put<uint64_t>(bytes, 0x20, extension ? (3ull << 48) | 70 : 0);
    put<uint16_t>(bytes, 48, 0x1234); put<uint16_t>(bytes, 50, 0x5678); put<uint16_t>(bytes, 52, 0x90AB);
    put<uint16_t>(bytes, 510, 0x1234); put<uint16_t>(bytes, 1022, 0x1234);
    const std::wstring name = L"python3.14.exe"; auto attr_length = (24 + 66 + name.size() * 2 + 7) & ~size_t(7);
    put<uint32_t>(bytes, 56, 0x30); put<uint32_t>(bytes, 60, static_cast<uint32_t>(attr_length));
    put<uint32_t>(bytes, 72, static_cast<uint32_t>(66 + name.size() * 2)); put<uint16_t>(bytes, 76, 24);
    put<uint64_t>(bytes, 80, (1ull << 48) | 5); bytes[80 + 64] = static_cast<uint8_t>(name.size()); bytes[80 + 65] = 1;
    memcpy(bytes.data() + 80 + 66, name.data(), name.size() * 2);
    put<uint32_t>(bytes, 56 + attr_length, UINT32_MAX); put<uint32_t>(bytes, 0x18, static_cast<uint32_t>(60 + attr_length));
    return bytes;
}
std::shared_ptr<wl::Volume> fixture() {
    auto volume = std::make_shared<wl::Volume>("X:\\"); volume->root_id = 5; volume->serial = 123;
    volume->add_initial({5, 5, 0, 0, 0, 0, FILE_ATTRIBUTE_DIRECTORY, "."});
    volume->add_initial({6, 5, 0, 0, 0, 0, FILE_ATTRIBUTE_DIRECTORY, "Python"});
    volume->add_initial({7, 6, 10000, 100, 50, 90, FILE_ATTRIBUTE_ARCHIVE, "python.exe"});
    volume->add_initial({7, 6, 10000, 100, 50, 90, FILE_ATTRIBUTE_ARCHIVE, "python3.exe"});
    volume->add_initial({8, 6, 500, 200, 50, 90, FILE_ATTRIBUTE_HIDDEN, "README.txt"});
    volume->add_initial({9, 5, 700, 300, 50, 90, 0, "中文😀.txt"});
    volume->add_initial({10, 5, 100, 400, 50, 90, 0, "PYTHON.EXE"}); volume->finish(); return volume;
}
wl::Reply search(wl::Engine& engine, std::vector<std::string> patterns, uint32_t flags = 0, uint64_t limit = UINT64_MAX) {
    wl::Request request; request.patterns = std::move(patterns); request.flags = flags; request.limit = limit; request.sort = wl::Sort::None;
    return engine.search(request);
}
void query_tests() {
    require(wl::glob_match("python*.exe", "python3.14.exe"), "glob prefix and suffix");
    require(!wl::glob_match("python*.exe", "python.exe.bak"), "glob is anchored");
    require(wl::glob_match("[!a-z]?", "9😀"), "Unicode glob and negated range");
    require(!wl::glob_match("[a-c]", "z"), "range rejection");
    require(wl::glob_match("a*b*c", "axbybc"), "glob backtracking");
    require(wl::glob_match("a[", "a["), "unclosed bracket is literal");
    require(wl::fold("ÄBC中文") == "äbc中文", "Unicode case folding");
    wl::Engine engine; auto volume = fixture(); engine.set_volumes({volume});
    require(search(engine, {"python*.exe"}).total == 3, "case insensitive glob and hard links");
    require(search(engine, {"python*.exe"}, wl::MatchCase).total == 2, "case sensitive glob");
    require(search(engine, {"python*.exe"}, 0, 1).results.size() == 1, "server limit");
    require(search(engine, {"*"}, 0, 0).total == 0, "zero limit");
    require(search(engine, {"Python"}).total == 5, "plain substring searches full paths");
    require(search(engine, {"Python"}, wl::Basename).total == 4, "basename literal");
    require(search(engine, {"*.exe", "*.txt"}).total == 5, "multiple patterns OR");
    require(search(engine, {"python*", "*.exe"}, wl::AllPatterns).total == 3, "multiple patterns AND");
    require(search(engine, {"*中文?.txt"}).total == 1, "non BMP wildcard");
    require(search(engine, {"python"}, wl::EverythingSyntax).total == 4, "Everything name substring");
    require(search(engine, {"ext:exe !case:PYTHON"}, wl::EverythingSyntax).total == 2, "extension and negative case term");
    require(search(engine, {"file: ext:txt;exe size:>1kb"}, wl::EverythingSyntax).total == 2, "metadata filters");
    require(search(engine, {"folder:"}, wl::EverythingSyntax).total == 1, "folder filter excludes root");
    require(search(engine, {"attrib:h"}, wl::EverythingSyntax).total == 1, "attribute filter");
    require(search(engine, {"parent:X:\\Python ext:txt"}, wl::EverythingSyntax).total == 1, "parent match");
    require(search(engine, {"README | 中文"}, wl::EverythingSyntax).total == 2, "Everything OR");
    require(search(engine, {"<README | python> ext:txt"}, wl::EverythingSyntax).total == 1, "grouped OR then AND");
    require(search(engine, {"!<README | python> file:"}, wl::EverythingSyntax).total == 1, "negated group");
    require(search(engine, {"<README | <python file:>> ext:exe"}, wl::EverythingSyntax).total == 3, "nested groups");
    require(search(engine, {"python | README", "ext:txt"}, wl::EverythingSyntax).total == 1, "independent filter applies to every OR branch");
    require(search(engine, {"^python(?:3)?\\.exe$", "ext:exe"}, wl::EverythingSyntax | wl::Regex).total == 3, "global GUI regex and independent filter");
    require(search(engine, {"regex:\"^python(?:3)?\\.exe$\""}, wl::EverythingSyntax).total == 3, "regex modifier preserves colons and quoted operators");
    rejects([&] { search(engine, {"<python"}, wl::EverythingSyntax); }, "unclosed group rejected");
    require(search(engine, {"^python[0-9]*\\.exe$"}, wl::Regex).total == 3, "regex matches names");
    rejects([&] { search(engine, {"["}, wl::Regex); }, "invalid regex");
    require(search(engine, {"(?<=中文)\\p{So}\\.txt$"}, wl::Regex).total == 1, "PCRE2 Unicode property and lookbehind");
    require(search(engine, {"^(python)(?:3)?\\.exe$"}, wl::Regex).total == 3, "PCRE2 captures do not change match success");
    wl::Pattern unicode_case("äbc", false); require(unicode_case.matches("ÄBC"), "PCRE2 Unicode caseless match");
    wl::Pattern bounded("(*NO_START_OPT)(*NO_AUTO_POSSESS)^(a+)+$", true);
    rejects([&] { bounded.matches(std::string(300, 'a') + "!"); }, "catastrophic regex stops at bounded work");
    rejects([&] { search(engine, {"size:foobar"}, wl::EverythingSyntax); }, "invalid metadata filter");
    rejects([&] { search(engine, {"dm:2026-02-30"}, wl::EverythingSyntax); }, "invalid calendar date rejected");
    rejects([&] { search(engine, {"dm:2026-01-01junk"}, wl::EverythingSyntax); }, "date trailing characters rejected");
    SYSTEMTIME today{}; GetLocalTime(&today); today.wHour = 12; today.wMinute = today.wSecond = today.wMilliseconds = 0;
    SYSTEMTIME utc{}; FILETIME ft{}; TzSpecificLocalTimeToSystemTime(nullptr, &today, &utc); SystemTimeToFileTime(&utc, &ft);
    wl::Result dated; dated.modified = wl::filetime_value(ft);
    wl::Request day; day.flags = wl::EverythingSyntax; day.patterns = {"dm:>today"};
    require(wl::Query(day).evaluate("f", "f", &dated) == wl::Truth::No, "date greater-than excludes entire named day");
    day.patterns = {"dm:<=today"}; require(wl::Query(day).evaluate("f", "f", &dated) == wl::Truth::Yes, "date less-or-equal includes entire named day");
    rejects([&] { search(engine, {"\"unterminated"}, wl::EverythingSyntax); }, "invalid quotes");
    auto before = volume->generation;
    volume->replace_links(6, {{6, 5, 0, 0, 0, 0, FILE_ATTRIBUTE_DIRECTORY, "Renamed"}});
    auto moved = search(engine, {"python3.exe"}); require(moved.results[0].path == "X:\\Renamed\\python3.exe", "directory rename updates descendant paths");
    require(volume->generation > before, "update invalidates cache generation");
    volume->replace_links(7, {}); require(search(engine, {"python*.exe"}, wl::MatchCase).total == 0, "deletion removes every hardlink");
    volume->replace_links(11, {{11, 5, 400, 0, 0, 0, 0, "new.exe"}}); require(search(engine, {"new*.exe"}).total == 1, "new unique name participates in prefix query");
    wl::Request sorted; sorted.flags = wl::EverythingSyntax; sorted.patterns = {"ext:txt;exe"}; sorted.sort = wl::Sort::Size; sorted.limit = 1;
    auto page = engine.search(sorted); require(page.total == 4 && page.results.size() == 1 && wl::filename(page.results[0].path) == "PYTHON.EXE", "sorted query reports full count and one row");
    sorted.offset = 1; auto next = engine.search(sorted); require(wl::filename(next.results[0].path) == "new.exe", "cached pagination");
    sorted.offset = UINT64_MAX; require(engine.search(sorted).results.empty(), "out-of-range pagination avoids overflow"); sorted.offset = 1;
    rejects([&] { engine.search(sorted, [] { return true; }); }, "cancelled cached search");
    volume->add_initial({20, 99999, 1, 0, 0, 0, 0, "orphan.txt"}); volume->finish();
    require(search(engine, {"orphan*"}).total == 0, "orphan rows do not inflate counts");
    auto names_version = volume->name_generation;
    volume->replace_links(11, {{11, 5, 800, 10, 0, 0, 0, "new.exe"}});
    require(names_version == volume->name_generation, "metadata-only updates preserve name-query cache identity");
    auto unavailable = std::make_shared<wl::Volume>("Y:\\"); engine.set_volumes({volume, unavailable});
    auto partial = search(engine, {"new.exe"}); require(partial.total == 1 && !partial.message.empty(), "one indexing volume does not block ready volumes");
    engine.set_volumes({volume});
    volume->add_initial({21, 5, 0, 0, 0, 0, FILE_ATTRIBUTE_DIRECTORY, "Renamed-abc"});
    volume->add_initial({22, 21, 1, 0, 0, 0, 0, "new.exe"}); volume->finish();
    wl::Request path_sort; path_sort.patterns = {"*"}; path_sort.sort = wl::Sort::Path;
    auto path_rows = engine.search(path_sort).results; auto expected = path_rows;
    std::stable_sort(expected.begin(), expected.end(), [](const auto& a, const auto& b) { return wl::fold(a.path) < wl::fold(b.path); });
    require(std::equal(path_rows.begin(), path_rows.end(), expected.begin(), [](const auto& a, const auto& b) { return a.path == b.path; }), "parent-interned path keys preserve full-path lexical ordering");
    auto names_before = volume->names.size(); volume->compact();
    require(volume->names.size() < names_before, "compaction releases unused interned names");
    require(search(engine, {"new.exe"}, wl::Basename).total == 2, "compaction preserves active paths and hardlinks");
    const std::vector<std::string> samples{"abc.exe", "中文😀.txt", "!HOW_TO.md", "]abcfoo", "file123.cpp", "ÄBCDEF.txt"};
    const std::vector<std::string> patterns{"*abc*", "?bc*", "*.exe", "[a-z]*", "[]abc]foo", "*中文?*", "*abc?ef*", "[!x]*txt", "*HOW_TO*"};
    for (auto& pattern : patterns) for (auto& sample : samples) {
        wl::Request r; r.flags = wl::Basename; r.patterns = {pattern}; wl::Query q(r);
        auto lower = wl::fold(sample); if (q.evaluate(sample, lower) == wl::Truth::Yes) require(q.may_match_name(wl::name_bloom(lower)), "Bloom filter must not reject a matching glob");
    }
}
void path_subtree_tests() {
    auto v = std::make_shared<wl::Volume>("X:\\"); v->root_id = 5; v->serial = 123;
    auto add = [&](uint64_t id, uint64_t parent, std::string name, bool dir = false) {
        v->add_initial({id, parent, 100, 200, 50, 90, uint32_t(dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE), std::move(name)});
    };
    add(5, 5, ".", true); add(6, 5, "Windows", true); add(7, 6, "Logs", true);
    add(8, 7, "a.log"); add(9, 7, "b.LOG"); add(10, 7, "nested", true);
    add(11, 10, "中文😀.log"); add(12, 7, "other.txt"); add(13, 6, "Logs-old", true); add(14, 13, "outside.log");
    add(15, 5, "Other", true); add(8, 15, "hardlink.log");
    add(16, 5, "windows", true); add(17, 16, "logs", true); add(18, 17, "case.log");
    add(19, 999, "orphan.log"); add(20, 7, "[literal.log"); add(21, 7, "empty", true); add(22, 7, "extra.log");
    v->finish(); wl::Engine engine; engine.set_volumes({v});
    // Compare the optimizer against a deliberately exhaustive full-path oracle.
    // This catches pruning mistakes without duplicating the subtree algorithm.
    auto compare = [&](const wl::Request& request) {
        wl::Query query(request); std::vector<std::string> expected, actual;
        for (uint32_t row = 0; row < v->entries.size(); ++row) {
            const auto& e = v->entries[row]; if (e.id == v->root_id || (e.attributes & wl::deleted_attribute)) continue;
            auto value = v->result(row); const auto& name = v->names[e.name];
            if (!value.path.empty() && query.evaluate(name.text, name.lower, &value) == wl::Truth::Yes) expected.push_back(value.path);
        }
        auto reply = engine.search(request);
        for (const auto& result : reply.results) actual.push_back(result.path);
        require(reply.total == expected.size(), "path plan preserves exhaustive count");
        if (request.sort == wl::Sort::Name)
            require(std::is_sorted(actual.begin(), actual.end(), [](const auto& a, const auto& b) { return wl::fold(wl::filename(a)) < wl::fold(wl::filename(b)); }), "subtree name ordering");
        std::sort(expected.begin(), expected.end()); std::sort(actual.begin(), actual.end());
        require(expected == actual, "path plan preserves exhaustive result set without duplicates");
    };
    const std::vector<std::string> patterns{
        "X:\\Windows\\Logs\\*.log", "x:\\windows\\logs\\*.log", "X:/Windows/Logs/*.log",
        "X:\\Windows\\Logs\\nested\\*.log", "X:\\Windows\\Logs\\empty\\*", "Y:\\Windows\\Logs\\*.log",
        "X:\\missing\\*.log", "X:\\Windows\\Logs*\\*.log", "X:\\Windows\\Logs\\?*.log",
        "X:\\Windows\\Logs\\[ab].log", "X:\\Windows\\Logs\\[literal*", "X:\\*", "*.log",
        "*Windows\\Logs\\*.log", "X:\\Windows\\Logs\\*.log | X:\\Windows\\*.log",
        "X:\\Windows\\Logs\\*.log | X:\\Other\\*.log", "X:\\Windows\\Logs\\*.log | outside.log",
        "<X:\\Windows\\Logs\\* | X:\\Other\\*> ext:log !b.LOG",
        "X:\\Windows\\Logs\\*.log file: size:>50", "!X:\\Windows\\Logs\\*.log",
        "path:X:\\Windows\\Logs\\*.log", "parent:X:\\Windows\\Logs*", "regex:.*log$"
    };
    for (auto flags : {uint32_t(wl::EverythingSyntax), uint32_t(wl::EverythingSyntax | wl::MatchCase)}) {
        for (const auto& pattern : patterns) for (auto sort : {wl::Sort::None, wl::Sort::Name, wl::Sort::Path}) {
            wl::Request request; request.flags = flags; request.patterns = {pattern}; request.sort = sort; compare(request);
        }
    }
    wl::Request r; r.patterns = {"X:\\Windows\\Logs\\*.log", "X:\\Other\\*.log"}; r.sort = wl::Sort::None;
    compare(r); r.flags = wl::AllPatterns; compare(r);
    r.patterns = {"X:\\Windows\\Logs\\*.log"}; r.flags = wl::MatchCase; compare(r);
    auto query = wl::Query(r);
    require(v->find(query, 1, [] { return false; }).size() == 1, "bounded subtree CLI limit");
    require(v->find(query, 0, [] { return false; }).empty(), "bounded subtree zero limit");
    rejects([&] { v->find(query, UINT64_MAX, [] { return true; }); }, "bounded subtree cancellation");
    // Real-time mutations must update both old and new parent chains, including
    // hardlinks, directory moves, deleted parents and late-arriving parents.
    v->replace_links(10, {{10, 15, 0, 0, 0, 0, FILE_ATTRIBUTE_DIRECTORY, "moved"}}); compare(r);
    r.patterns = {"X:\\Other\\*.log"}; compare(r);
    v->replace_links(7, {{7, 6, 0, 0, 0, 0, FILE_ATTRIBUTE_DIRECTORY, "Renamed"}});
    r.patterns = {"X:\\Windows\\Logs\\*.log"}; compare(r);
    r.patterns = {"X:\\Windows\\Renamed\\*.log"}; compare(r);
    v->replace_links(8, {{8, 7, 100, 200, 50, 90, 0, "renamed.log"}, {8, 15, 100, 200, 50, 90, 0, "hardlink.log"}}); compare(r);
    v->replace_links(8, {{8, 15, 100, 200, 50, 90, 0, "hardlink.log"}}); compare(r);
    v->replace_links(7, {}); compare(r);
    v->replace_links(7, {{7, 6, 0, 0, 0, 0, FILE_ATTRIBUTE_DIRECTORY, "Restored"}});
    r.patterns = {"X:\\Windows\\Restored\\*.log"}; compare(r);
    v->replace_links(999, {{999, 6, 0, 0, 0, 0, FILE_ATTRIBUTE_DIRECTORY, "late"}});
    r.patterns = {"X:\\Windows\\late\\*.log"}; compare(r);
    v->compact(); compare(r);
    auto path = std::filesystem::temp_directory_path() / (L"wlocate-path-test-" + std::to_wstring(GetCurrentProcessId()) + L".widx");
    v->save(path); v = wl::Volume::load(path, "X:\\", 123); engine.set_volumes({v}); compare(r); std::filesystem::remove(path);
    for (uint64_t i = 0; i < 30000; ++i) add(1000 + i, 15, "unrelated" + std::to_string(i) + ".log");
    v->finish();
    r.patterns = {"X:\\Windows\\Restored\\*.log"}; r.sort = wl::Sort::Name;
    unsigned polls = 0;
    auto small = engine.search(r, [&] { return ++polls > 8; });
    require(!small.results.empty(), "small subtree completes bounded work despite 30000 unrelated names");
    auto second_volume = fixture(); second_volume->root = "Y:\\"; engine.set_volumes({v, second_volume});
    r.patterns = {"X:\\Windows\\Restored\\*.log", "Y:\\Python\\*.exe"};
    auto multi = engine.search(r);
    require(multi.total == small.total + 2 && std::is_sorted(multi.results.begin(), multi.results.end(), [](const auto& a, const auto& b) {
        return wl::fold(wl::filename(a.path)) < wl::fold(wl::filename(b.path));
    }), "bounded OR across volumes merges name ordering");
    r.flags = wl::EverythingSyntax | wl::MatchCase; r.patterns = {"X:\\Windows\\Restored\\*.log"}; r.limit = 1;
    auto first = engine.search(r); r.offset = 1; auto second = engine.search(r);
    require(first.total == small.total && second.total == small.total && first.results.size() == 1 && second.results.size() == 1 && first.results[0].path != second.results[0].path,
        "GUI subtree pagination preserves full count and cached row offsets");
}
void relative_path_tests() {
    auto v = std::make_shared<wl::Volume>("X:\\"); v->root_id = 5; v->serial = 345;
    auto add = [&](uint64_t id, uint64_t parent, const std::string& name, bool dir = false) {
        v->add_initial({id, parent, 123, 456, 50, 90, uint32_t(dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE), name});
    };
    add(5, 5, ".", true); add(6, 5, "foo", true); add(7, 6, "bar", true); add(8, 7, "child.txt");
    add(9, 6, "barista.txt"); add(10, 5, "afoo", true); add(11, 10, "bargain", true); add(12, 11, "other.txt");
    add(13, 5, "foox", true); add(14, 13, "bar-no.txt"); add(15, 6, "nested", true); add(16, 15, "bar-no.txt");
    add(17, 7, "foo", true); add(18, 17, "bar", true); add(19, 18, "deep.txt"); // overlapping seeds
    add(20, 5, "FOO", true); add(21, 20, "BAR", true); add(22, 21, "case.txt");
    add(23, 5, "Other", true); add(8, 23, "hardlink.txt"); add(8, 7, "second-link.txt");
    add(24, 999, "bar-orphan.txt"); add(25, 5, "中文😀", true); add(26, 25, "ÄBC", true); add(27, 26, "emoji😀.txt");
    add(28, 5, "space dir", true); add(29, 28, "bar with space", true); add(30, 29, "file.txt");
    v->finish(); wl::Engine engine; engine.set_volumes({v});
    auto compare = [&](const wl::Request& r) {
        wl::Query q(r); std::vector<std::string> expected, actual;
        for (uint32_t row = 0; row < v->entries.size(); ++row) {
            const auto& e = v->entries[row];
            if (e.id == v->root_id || (e.attributes & wl::deleted_attribute)) continue;
            auto value = v->result(row); const auto& name = v->names[e.name];
            if (!value.path.empty() && q.evaluate(name.text, name.lower, &value) == wl::Truth::Yes) expected.push_back(value.path);
        }
        auto reply = engine.search(r);
        require(reply.total == expected.size(), "relative path plan count matches exhaustive full-path oracle");
        for (const auto& value : reply.results) actual.push_back(value.path);
        if (r.flags & wl::CountOnly) { require(actual.empty(), "relative path count-only returns no rows"); return; }
        if (r.sort == wl::Sort::Name) require(std::is_sorted(actual.begin(), actual.end(), [](const auto& a, const auto& b) {
            return wl::fold(wl::filename(a)) < wl::fold(wl::filename(b));
        }), "relative path results remain name sorted");
        std::sort(expected.begin(), expected.end()); std::sort(actual.begin(), actual.end());
        if (actual != expected) throw wl::Error("Relative path result mismatch: " + r.patterns[0]);
        require(actual == expected, "relative path plan neither misses nor duplicates paths");
    };
    const std::vector<std::string> patterns{
        "foo\\bar", "FOO\\BAR", "oo\\ba", "foo/bar", "foo\\", "oo\\", "foo\\bar\\", "foo\\bar\\ch",
        "foo\\bar\\foo\\ba", "foo\\nested\\ba", "foo\\bar\\nonexistent", "foo\\missing", "missing\\bar",
        "\\bar", "\\foo\\", "\\foo\\bar", "X:\\foo", "x:\\foo", "X:\\", ":\\", "\\", "\\\\", "foo\\\\bar",
        "中文😀\\ä", "中文😀/ÄBC/emoji😀", "文😀\\ÄBC\\", "X:\\中文😀\\Ä", "space dir\\bar", "space dir\\bar with space\\"
    };
    for (auto flags : {uint32_t(0), uint32_t(wl::MatchCase), uint32_t(wl::EverythingSyntax), uint32_t(wl::EverythingSyntax | wl::MatchCase)}) {
        for (const auto& pattern : patterns) for (auto sort : {wl::Sort::None, wl::Sort::Name, wl::Sort::Path}) {
            if ((flags & wl::EverythingSyntax) && pattern.starts_with(':')) continue; // Bare ':' is a syntax operator in this mode.
            wl::Request r; r.flags = flags; r.sort = sort; r.patterns = {flags & wl::EverythingSyntax ? "\"" + pattern + "\"" : pattern}; compare(r);
        }
    }
    for (const std::string pattern : {
        "foo\\bar ext:txt !second-link", "<foo\\bar | 中文😀\\Ä> file:", "foo\\bar | oo\\bar", "foo\\bar | foo\\bar\\foo\\bar",
        "foo\\bar | hardlink", "!foo\\bar", "ww:foo\\bar", "ww:foo\\", "case:foo\\bar", "path:foo\\bar size:>100",
        "foo\\bar parent:foo", "foo\\bar !path:nested", "foo\\bar | X:\\Other\\*", "foo\\bar\\foo foo\\bar", "parent:foo\\bar"
    }) {
        wl::Request r; r.flags = wl::EverythingSyntax; r.patterns = {pattern}; r.sort = wl::Sort::Name; compare(r);
    }
    // Draw fragments through arbitrary component boundaries, independent of
    // how the query planner chooses an anchor.
    std::vector<std::string> sample_paths;
    for (uint32_t row = 0; row < v->entries.size(); ++row) {
        auto path = v->full_path(row);
        if (std::all_of(path.begin(), path.end(), [](unsigned char c) { return c < 128; })) sample_paths.push_back(std::move(path));
    }
    for (const auto& path : sample_paths) for (size_t start = 0; start < path.size(); start += 2) {
        for (size_t length = 2; length <= path.size() - start; length += 3) {
            auto fragment = path.substr(start, length); if (fragment.find('\\') == fragment.npos) continue;
            wl::Request r; r.flags = wl::MatchCase; r.sort = wl::Sort::None; r.patterns = {fragment}; compare(r);
        }
    }
    wl::Request r; r.patterns = {"foo\\bar", "中文😀\\Ä"}; r.sort = wl::Sort::Name; compare(r);
    r.flags = wl::AllPatterns; compare(r); r.flags = wl::CountOnly; compare(r);
    r.flags = wl::EverythingSyntax; r.patterns = {"foo\\bar"};
    auto total = engine.search(r).total;
    r.limit = 1; auto first = engine.search(r); r.offset = 1; auto second = engine.search(r);
    require(first.total == total && second.total == total && first.results.size() == 1 && second.results.size() == 1 && first.results[0].path != second.results[0].path,
        "relative path cached GUI pagination preserves full count");
    r.offset = 0; r.flags = wl::MatchCase; r.sort = wl::Sort::None;
    require(engine.search(r).results.size() == 1, "relative path CLI early limit");
    r.limit = 0; require(engine.search(r).results.empty(), "relative path zero limit"); r.limit = UINT64_MAX;
    rejects([&] { v->find(wl::Query(r), UINT64_MAX, [] { return true; }); }, "relative path cancellation");
    // Keep querying the same text while name generations and candidate chains change.
    auto verify_mutation = [&] {
        for (const std::string pattern : {"foo\\bar", "afoo\\bar", "renamed\\bar", "foo\\", "中文😀\\Ä"}) { r.patterns = {pattern}; compare(r); }
    };
    v->replace_links(7, {{7, 10, 0, 0, 0, 0, FILE_ATTRIBUTE_DIRECTORY, "bar-moved"}}); verify_mutation();
    v->replace_links(10, {{10, 5, 0, 0, 0, 0, FILE_ATTRIBUTE_DIRECTORY, "renamed"}}); verify_mutation();
    v->replace_links(8, {{8, 6, 123, 456, 50, 90, 0, "bar-new-link.txt"}, {8, 23, 123, 456, 50, 90, 0, "hardlink.txt"}}); verify_mutation();
    v->replace_links(999, {{999, 5, 0, 0, 0, 0, FILE_ATTRIBUTE_DIRECTORY, "foo"}}); verify_mutation();
    v->replace_links(999, {}); verify_mutation();
    v->compact(); verify_mutation();
    auto snapshot = std::filesystem::temp_directory_path() / (L"wlocate-relative-test-" + std::to_wstring(GetCurrentProcessId()) + L".widx");
    v->save(snapshot); v = wl::Volume::load(snapshot, "X:\\", 345); engine.set_volumes({v}); verify_mutation(); std::filesystem::remove(snapshot);
    for (uint64_t i = 0; i < 30000; ++i) add(1000 + i, 23, "unrelated" + std::to_string(i) + ".txt"); v->finish();
    for (const std::string pattern : {"foo\\bar", "foo\\no-such-name", "renamed\\bar\\"}) {
        r.patterns = {pattern}; r.sort = wl::Sort::Name; unsigned polls = 0;
        engine.search(r, [&] { return ++polls > 8; }); require(true, "relative path avoids scanning 30000 unrelated names");
    }
}
void parser_tests() {
    auto bytes = record_fixture(true); require(wl::fixup_record(bytes, 512), "MFT sequence fixup");
    require(bytes[510] == 0x78 && bytes[511] == 0x56, "sector tail restored");
    auto record = wl::parse_record(bytes, 100);
    require(record.id == ((7ull << 48) | 100), "FRN includes sequence");
    require(record.base_id == ((3ull << 48) | 70), "extension retains base FRN");
    require(record.names.size() == 1 && record.names[0].second == "python3.14.exe", "extension filename is preserved");
    uint32_t attr_size; memcpy(&attr_size, bytes.data() + 60, sizeof(attr_size));
    auto alias = std::vector<uint8_t>(bytes.begin() + 56, bytes.begin() + 56 + attr_size); alias[24 + 65] = 2;
    memcpy(bytes.data() + 56 + attr_size, alias.data(), alias.size());
    put<uint32_t>(bytes, 56 + 2 * attr_size, UINT32_MAX); put<uint32_t>(bytes, 0x18, 60 + 2 * attr_size); put<uint16_t>(bytes, 0x12, 2);
    auto with_dos = wl::parse_record(bytes, 100);
    require(with_dos.link_count == 2 && with_dos.filename_attribute_count == 2 && with_dos.names.size() == 1, "DOS aliases count toward MFT completeness but are not search results");
    bytes = record_fixture(false); bytes[510] = 0; require(!wl::fixup_record(bytes, 512), "torn record rejected");
    bytes = record_fixture(false); wl::fixup_record(bytes, 512); put<uint32_t>(bytes, 60, UINT32_MAX);
    rejects([&] { wl::parse_record(bytes, 1); }, "invalid attribute rejected");
    std::vector<uint8_t> data{0x11, 3, 100, 0x11, 2, 0xF6, 0x01, 1, 0}; auto runs = wl::parse_runs(data, 0);
    require(runs.size() == 3 && runs[0].lcn == 100 && runs[1].lcn == 90 && runs[2].lcn == -1 && runs[2].vcn == 5, "signed and sparse runs");
    data = {0x91, 1, 2}; rejects([&] { wl::parse_runs(data, 0); }, "invalid run rejected");
}
void snapshot_tests() {
    auto path = std::filesystem::temp_directory_path() / (L"wlocate-test-" + std::to_wstring(GetCurrentProcessId()) + L".widx");
    auto volume = fixture(); volume->journal_id = 456; volume->next_usn = 789; volume->save(path);
    auto restored = wl::Volume::load(path, "X:\\", 123); require(restored && restored->files == volume->files && restored->next_usn == 789, "snapshot restores counters and journal");
    wl::Engine engine; engine.set_volumes({restored}); require(search(engine, {"python*.exe"}).total == 3, "snapshot restores hardlinks and name lookup");
    require(!wl::Volume::load(path, "X:\\", 124), "wrong volume serial rejected");
    { std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary); file.seekg(-24, std::ios::end); char byte; file.get(byte); byte ^= 1; file.seekp(-24, std::ios::end); file.put(byte); }
    rejects([&] { wl::Volume::load(path, "X:\\", 123); }, "snapshot detects silent data corruption");
    volume->checkpoint_usn = 700; volume->save(path);
    restored = wl::Volume::load(path, "X:\\", 123); require(restored->next_usn == 700, "snapshot replays unresolved journal updates after restart");
    { std::ofstream file(path, std::ios::trunc | std::ios::binary); file << "bad"; }
    rejects([&] { wl::Volume::load(path, "X:\\", 123); }, "truncated snapshot rejected");
    std::filesystem::remove(path);
}
}
int main() {
    try { encoding_tests(); query_tests(); path_subtree_tests(); relative_path_tests(); parser_tests(); snapshot_tests(); std::cout << checks << " checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << "After " << checks << " checks: " << error.what() << '\n'; return 1; }
}
