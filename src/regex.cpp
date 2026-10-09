#include "regex.hpp"
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

namespace wl {
namespace {
struct MatchState {
    pcre2_match_data* data = pcre2_match_data_create(1, nullptr);
    pcre2_match_context* context = pcre2_match_context_create(nullptr);
    pcre2_jit_stack* stack = pcre2_jit_stack_create(32768, 256 * 1024, nullptr);
    MatchState() {
        if (!data || !context) {
            pcre2_match_data_free(data); pcre2_match_context_free(context); pcre2_jit_stack_free(stack);
            throw Error("Cannot allocate regex match state");
        }
        pcre2_set_match_limit(context, 100000); pcre2_set_depth_limit(context, 1000); pcre2_set_heap_limit(context, 8192);
        if (stack) pcre2_jit_stack_assign(context, nullptr, stack);
    }
    ~MatchState() { pcre2_match_data_free(data); pcre2_match_context_free(context); pcre2_jit_stack_free(stack); }
};
std::string message(int error) {
    PCRE2_UCHAR buffer[256]{}; pcre2_get_error_message(error, buffer, sizeof(buffer)); return reinterpret_cast<char*>(buffer);
}
}
Pattern::Pattern(std::string_view expression, bool match_case) {
    int error = 0; PCRE2_SIZE offset = 0;
    code_ = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(expression.data()), expression.size(), PCRE2_UTF | PCRE2_UCP | (match_case ? 0 : PCRE2_CASELESS), &error, &offset, nullptr);
    if (!code_) throw Error("Invalid regular expression at byte " + std::to_string(offset) + ": " + message(error));
    // Unsupported JIT patterns remain usable through PCRE2's bounded interpreter.
    pcre2_jit_compile(static_cast<pcre2_code*>(code_), PCRE2_JIT_COMPLETE);
}
Pattern::~Pattern() { pcre2_code_free(static_cast<pcre2_code*>(code_)); }
bool Pattern::matches(std::string_view text) const {
    thread_local MatchState state;
    auto rc = pcre2_match(static_cast<pcre2_code*>(code_), reinterpret_cast<PCRE2_SPTR>(text.data()), text.size(), 0, 0, state.data, state.context);
    if (rc == PCRE2_ERROR_NOMATCH) return false;
    if (rc < 0) throw Error("Regular expression: " + message(rc));
    return true;
}
}
