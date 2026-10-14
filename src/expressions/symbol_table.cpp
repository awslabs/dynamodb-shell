// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// symbol_table.cpp — see symbol_table.hpp.

#include "expressions/symbol_table.hpp"

namespace ddbsh::engine {

std::string SymbolTable::new_attribute(const std::string& name) {
    // Tokenize on '.' (map/document path separators), aliasing each segment.
    // Reproduces the original CSymbolTable::new_attribute loop exactly,
    // including the trailing-token handling when there is no final '.'.
    std::string token;
    std::size_t start = 0;
    std::size_t end = 0;
    do {
        end = name.find('.', start);
        std::string segment =
            (end == std::string::npos) ? name.substr(start) : name.substr(start, end - start);

        if (token.empty()) {
            token = new_segment(segment);
        } else {
            token = token + "." + new_segment(segment);
        }

        start = end + 1;
    } while (end != std::string::npos);

    return token;
}

std::string SymbolTable::new_segment(const std::string& name) {
    // A segment may carry a `[subscript]` suffix; alias only the bare name and
    // keep the subscript verbatim on the token (e.g. `a[3]` -> `#a<p>1[3]`).
    std::size_t bracket = name.find('[');
    if (bracket != std::string::npos) {
        return new_bare(name.substr(0, bracket)) + name.substr(bracket);
    }
    return new_bare(name);
}

std::string SymbolTable::new_bare(const std::string& name) {
    std::string token = "#a" + prefix_ + std::to_string(attr_ix_);
    auto [it, inserted] = names_.emplace(name, token);
    if (inserted) {
        ++attr_ix_;
    } else {
        token = it->second;  // reuse the existing token for a repeated name
    }
    return token;
}

std::string SymbolTable::new_value(Value value) {
    std::string token = ":v" + prefix_ + std::to_string(value_ix_++);
    values_.emplace(token, std::move(value));
    return token;
}

std::map<std::string, std::string> SymbolTable::get_names() const {
    std::map<std::string, std::string> inverted;
    for (const auto& [real_name, token] : names_) {
        inverted.emplace(token, real_name);
    }
    return inverted;
}

}  // namespace ddbsh::engine
