#pragma once

#include "../ast/declaration.h"
#include "../optimizer/optimizer.h"
// TODO: unused here; tests/io.cpp calls resolve_builtins without including this itself. Include
// it there and drop it from this header.
#include "../semantic/builtins.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using struct_impls_t = std::unordered_map<std::string, std::vector<const function_decl *>>;
using state_invs_t = std::unordered_map<std::string, std::vector<const invariant_decl *>>;

// Translates an analysed program to C++ source lines. The AST visitor that does the work lives
// in code_generator.cpp and is created afresh by every gen_code call, so no per-run state
// (output, scopes, step counters) can leak from one call into the next.
class code_generator {
    struct_impls_t impls; // Struct name to borrowed method declarations.
    // TODO: invs is never read: invariants are emitted from their invariant_decl nodes. Drop
    // this parameter together with its callers (src/main.cpp, tests/optimizer.cpp,
    // tests/io.cpp, tests/names.cpp) unless a planned use needs it.
    state_invs_t invs; // State name to borrowed invariant declarations.

  public:
    // Taken by value: callers passing lvalues get a copy as before, and callers that no longer
    // need the indexes can move them in.
    code_generator(struct_impls_t method_impls, state_invs_t state_invariants)
        : impls(std::move(method_impls)), invs(std::move(state_invariants)) {
    }

    std::vector<std::string> gen_code(const program &node, optimization_options options = {}) const;
};
