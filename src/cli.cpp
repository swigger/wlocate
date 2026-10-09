#include "ipc.hpp"
#include <cstdio>

namespace {
std::atomic<bool> interrupted = false;
std::atomic<HANDLE> cancel_event = nullptr;
BOOL WINAPI interrupt(DWORD code) { if (code == CTRL_C_EVENT || code == CTRL_BREAK_EVENT) { interrupted = true; if (auto event = cancel_event.load()) SetEvent(event); return TRUE; } return FALSE; }
void output(HANDLE handle, const std::string& text) {
    DWORD mode;
    if (GetConsoleMode(handle, &mode)) {
        auto unicode = wl::wide(text);
        for (size_t at = 0; at < unicode.size();) {
            DWORD done = 0; auto length = static_cast<DWORD>(std::min<size_t>(16000, unicode.size() - at));
            if (at + length < unicode.size() && unicode[at + length - 1] >= 0xD800 && unicode[at + length - 1] <= 0xDBFF) --length;
            if (!WriteConsoleW(handle, unicode.data() + at, length, &done, nullptr) || !done) throw wl::Error("Cannot write console output"); at += done;
        }
    } else if (!wl::write_exact(handle, text.data(), text.size())) throw wl::Error("Cannot write output: " + wl::win_error());
}
uint64_t number(const std::wstring& value, std::wstring_view option) {
    if (value.empty() || value.find_first_not_of(L"0123456789") != value.npos) throw wl::Error("Invalid value for " + wl::utf8(option));
    size_t used; try { return std::stoull(value, &used); } catch (...) { throw wl::Error("Numeric option is out of range"); }
}
constexpr char help[] =
"Usage: locate [OPTIONS] PATTERN...\n"
"Search the native WLocate file index. Quote wildcard patterns in your shell.\n\n"
"  -n, -l, --limit N       Print at most N matches (limited in the service)\n"
"  -i, --ignore-case       Ignore case (default: case-sensitive)\n"
"  -b, --basename         Match only the filename\n"
"  -w, --wholename        Match the full path\n"
"  -A, -a, --all          Require every pattern (default: any pattern)\n"
"  -c, --count            Print the number of matches\n"
"  -0, --null             Separate paths with NUL instead of newline\n"
"  -e, --existing         Check that each result still exists\n"
"  -E, --non-existing     Return only missing paths\n"
"  -r, --regexp PATTERN   Search using Unicode PCRE2 regular expressions\n"
"      --regex            Treat all patterns as regular expressions\n"
"      --query TEXT       Everything-style query (name matching, ignores case)\n"
"      --sort FIELD       name, path, size, modified, created, extension\n"
"      --descending       Reverse the selected sort\n"
"      --timeout MS       Request timeout (100..600000, default 120000)\n"
"      --timing           Print service search time to stderr\n"
"  -S, --statistics      Show index status\n"
"      --reindex           Request a full rebuild (administrator required)\n"
"  -h, --help            Show this help\n"
"  -V, --version         Show the version\n\n"
"Without wildcards, patterns are full-path substrings. Bare wildcard patterns\n"
"match filenames; patterns containing a separator match full paths. Multiple\n"
"patterns use OR unless --all is set. Glob syntax: *, ?, [abc], [!abc].\n"
"Output is UTF-8 when redirected. Exit codes: 0 matches, 1 none, 2 error.\n"
"Examples: locate -n 10 \"python*.exe\"\n"
"          locate --query \"ext:cpp;h size:>1kb !path:build\"\n";
}
int wmain(int argc, wchar_t** argv) {
    bool quiet = false;
    try {
        wl::require_utf8_process();
        wl::Request request; request.flags = wl::MatchCase; request.sort = wl::Sort::None;
        bool nul = false, timing = false, options = true;
        auto argument = [&](int& index, const std::wstring& inline_value, std::wstring_view option) {
            if (!inline_value.empty()) return inline_value;
            if (++index >= argc) throw wl::Error("Missing argument for " + wl::utf8(option)); return std::wstring(argv[index]);
        };
        for (int i = 1; i < argc; ++i) {
            std::wstring arg = argv[i];
            if (options && arg == L"--") { options = false; continue; }
            if (!options || arg.empty() || arg[0] != L'-' || arg == L"-") { request.patterns.push_back(wl::utf8(arg)); continue; }
            auto equal = arg.find(L'='); std::wstring value;
            if (equal != arg.npos) { value = arg.substr(equal + 1); arg.resize(equal); }
            if (arg == L"--help" || arg == L"-h") { output(GetStdHandle(STD_OUTPUT_HANDLE), help); return 0; }
            if (arg == L"--version" || arg == L"-V") { output(GetStdHandle(STD_OUTPUT_HANDLE), "locate (WLocate) 1.0.0 native x64\n"); return 0; }
            if (arg == L"--ignore-case" || arg == L"-i") request.flags &= ~wl::MatchCase;
            else if (arg == L"--basename" || arg == L"-b") { request.flags |= wl::Basename; request.flags &= ~wl::MatchPath; }
            else if (arg == L"--wholename" || arg == L"-w") { request.flags |= wl::MatchPath; request.flags &= ~wl::Basename; }
            else if (arg == L"--all" || arg == L"-A" || arg == L"-a") request.flags |= wl::AllPatterns;
            else if (arg == L"--count" || arg == L"-c") request.flags |= wl::CountOnly;
            else if (arg == L"--null" || arg == L"-0") nul = true;
            else if (arg == L"--existing" || arg == L"-e") { request.flags |= wl::Existing; request.flags &= ~wl::NonExisting; }
            else if (arg == L"--non-existing" || arg == L"-E") { request.flags |= wl::NonExisting; request.flags &= ~wl::Existing; }
            else if (arg == L"--statistics" || arg == L"-S") request.operation = 1;
            else if (arg == L"--reindex") request.operation = 2;
            else if (arg == L"--timing") timing = true;
            else if (arg == L"--quiet" || arg == L"-q") quiet = true;
            else if (arg == L"--regex") request.flags |= wl::Regex;
            else if (arg == L"--regexp" || arg == L"-r") { request.flags |= wl::Regex; request.patterns.push_back(wl::utf8(argument(i, value, arg))); }
            else if (arg == L"--query") { request.flags |= wl::EverythingSyntax; request.flags &= ~wl::MatchCase; request.patterns.push_back(wl::utf8(argument(i, value, arg))); }
            else if (arg == L"--descending") request.descending = true;
            else if (arg == L"--timeout") { auto n = number(argument(i, value, arg), arg); if (n < 100 || n > 600000) throw wl::Error("Timeout must be 100..600000 ms"); request.timeout_ms = static_cast<uint32_t>(n); }
            else if (arg == L"--sort") {
                auto field = argument(i, value, arg);
                if (field == L"name") request.sort = wl::Sort::Name; else if (field == L"path") request.sort = wl::Sort::Path;
                else if (field == L"size") request.sort = wl::Sort::Size; else if (field == L"modified") request.sort = wl::Sort::Modified;
                else if (field == L"created") request.sort = wl::Sort::Created; else if (field == L"extension") request.sort = wl::Sort::Extension;
                else throw wl::Error("Unknown sort field");
            } else if (arg == L"--limit" || arg == L"-n" || arg == L"-l") request.limit = number(argument(i, value, arg), arg);
            else if (arg.size() > 2 && arg[0] == L'-' && (arg[1] == L'n' || arg[1] == L'l')) request.limit = number(arg.substr(2), arg.substr(0, 2));
            else if (arg.size() > 2 && arg[1] != L'-') {
                for (size_t c = 1; c < arg.size(); ++c) {
                    switch (arg[c]) { case L'i': request.flags &= ~wl::MatchCase; break; case L'c': request.flags |= wl::CountOnly; break;
                    case L'b': request.flags |= wl::Basename; break; case L'A': case L'a': request.flags |= wl::AllPatterns; break; case L'0': nul = true; break;
                    default: throw wl::Error("Unsupported combined option: " + wl::utf8(arg)); }
                }
            } else throw wl::Error("Unknown option: " + wl::utf8(arg));
        }
        if (!request.operation && request.patterns.empty()) { output(GetStdHandle(STD_ERROR_HANDLE), help); return 2; }
        SetConsoleCtrlHandler(interrupt, TRUE);
        wl::Handle cancellation(CreateEventW(nullptr, TRUE, FALSE, nullptr)); cancel_event = cancellation.value;
        auto reply = wl::request_service(request, cancellation.value);
        cancel_event = nullptr;
        if (interrupted) return 130;
        if (request.operation) { output(GetStdHandle(STD_OUTPUT_HANDLE), reply.message); return 0; }
        if (request.flags & wl::CountOnly) output(GetStdHandle(STD_OUTPUT_HANDLE), std::to_string(std::min(reply.total, request.limit)) + "\n");
        else {
            std::string buffer; buffer.reserve(65536);
            for (auto& row : reply.results) {
                if (interrupted) return 130;
                buffer += row.path; buffer += nul ? '\0' : '\n';
                if (buffer.size() >= 65536) { output(GetStdHandle(STD_OUTPUT_HANDLE), buffer); buffer.clear(); }
            }
            output(GetStdHandle(STD_OUTPUT_HANDLE), buffer);
        }
        if (timing) output(GetStdHandle(STD_ERROR_HANDLE), "search=" + std::to_string(reply.search_us / 1000.0) + " ms, matches=" + std::to_string(reply.total) + "\n");
        if (!reply.message.empty()) {
            if (!quiet) output(GetStdHandle(STD_ERROR_HANDLE), "locate: partial index: " + reply.message + "\n");
            return 2;
        }
        return reply.total ? 0 : 1;
    } catch (const std::exception& error) {
        if (!quiet) { try { output(GetStdHandle(STD_ERROR_HANDLE), std::string("locate: ") + error.what() + "\n"); } catch (...) {} }
        return interrupted ? 130 : 2;
    }
}
