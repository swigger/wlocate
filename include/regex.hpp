#pragma once
#include "common.hpp"

namespace wl {
class Pattern {
    void* code_ = nullptr;
public:
    Pattern(std::string_view expression, bool match_case);
    ~Pattern();
    Pattern(const Pattern&) = delete;
    Pattern& operator=(const Pattern&) = delete;
    bool matches(std::string_view text) const;
};
}
