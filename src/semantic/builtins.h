#pragma once

#include "../ast/statement.h"
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>

enum class builtin_function : std::uint8_t { print, scan };
using builtin_calls = std::unordered_map<const postfix_expr *, builtin_function>;

// A rejected built-in call. The resolver has no filename, so analyse() rethrows this as a
// semantic_error with the same code, span and detail. It stays an invalid_argument for
// callers that run the resolver without the checker (code_generator::gen_code).
class builtin_error : public std::invalid_argument {
  public:
    std::string code;
    source_span span;
    std::string detail;

    builtin_error(std::string code, source_span span, std::string detail);
};

// Resolve built-ins by lexical binding and validate their uses before optimisation.
// This focused pass tracks stream arguments and transitive I/O effects; semantic.cpp
// handles the general language rules. Calls are identified by AST node, not spelling.
// Throws builtin_error.
//
// TODO: this runs twice per compile, once inside analyse() and again in
// code_generator::gen_code. analysis_root could carry the builtin_calls the checker
// already computed, and the code generator take them from there; that changes
// semantic.h, main.cpp and the code generator together.
builtin_calls resolve_builtins(const program &node);
