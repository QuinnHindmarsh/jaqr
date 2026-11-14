#pragma once

#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// Structure and Function are non-owning handles (pointers into the AST), so a lookup can
// return a null handle for a missing name instead of wrapping the handle in an optional.
template <class Variable, class Structure, class Function> class symbol_table {
    static_assert(std::is_pointer_v<Structure> && std::is_pointer_v<Function>,
                  "struct and function symbols must be pointer handles");

    using scope = std::unordered_map<std::string, Variable>;
    std::vector<scope> scopes{1}; // The outermost scope holds global variables.
    std::unordered_map<std::string, Structure> structs;
    std::unordered_map<std::string, Function> functions;
    std::unordered_map<std::string, std::unordered_map<std::string, Function>> methods;

    void leave_scope() {
        scopes.pop_back();
    }
    // JAQR rejects shadowing visible variables and collisions across global categories.
    bool name_taken(const std::string &name) const {
        return lookup_variable(name) || structs.contains(name) || functions.contains(name);
    }

  public:
    // Tie scope removal to the visitor's lifetime, including exits caused by diagnostics.
    class scope_guard {
        symbol_table *owner;
        explicit scope_guard(symbol_table &table) : owner(&table) {
        }
        friend class symbol_table;

      public:
        scope_guard(const scope_guard &) = delete;
        scope_guard &operator=(const scope_guard &) = delete;
        scope_guard(scope_guard &&other) noexcept : owner(std::exchange(other.owner, nullptr)) {
        }
        ~scope_guard() {
            if (owner)
                owner->leave_scope();
        }
    };

    [[nodiscard]] scope_guard enter_scope() {
        scopes.emplace_back();
        return scope_guard(*this);
    }

    const Variable *lookup_variable(const std::string &name) const {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
            if (const auto found = it->find(name); found != it->end())
                return &found->second;
        return nullptr;
    }
    Structure lookup_struct(const std::string &name) const {
        const auto found = structs.find(name);
        return found == structs.end() ? nullptr : found->second;
    }
    Function lookup_function(const std::string &name) const {
        const auto found = functions.find(name);
        return found == functions.end() ? nullptr : found->second;
    }
    // The owner-then-method order mirrors `owner.method` in the source.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    Function lookup_method(const std::string &owner, const std::string &name) const {
        if (const auto group = methods.find(owner); group != methods.end())
            if (const auto found = group->second.find(name); found != group->second.end())
                return found->second;
        return nullptr;
    }

    // Methods have a separate namespace per owning struct, so only the other categories
    // check name_taken.
    bool declare_variable(std::string name, Variable symbol) {
        if (name_taken(name))
            return false;
        return scopes.back().emplace(std::move(name), std::move(symbol)).second;
    }
    bool declare_struct(std::string name, Structure symbol) {
        if (name_taken(name))
            return false;
        return structs.emplace(std::move(name), symbol).second;
    }
    bool declare_function(std::string name, Function symbol) {
        if (name_taken(name))
            return false;
        return functions.emplace(std::move(name), symbol).second;
    }
    bool declare_method(const std::string &owner, std::string name, Function symbol) {
        return methods[owner].emplace(std::move(name), symbol).second;
    }
};
