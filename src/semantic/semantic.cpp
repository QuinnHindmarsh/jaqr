#include "semantic.h"

#include "../symbol_table/symbol_table.h"
#include "../visitor/visitor.h"
#include "builtins.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

semantic_error::semantic_error(std::string filename, std::string code, source_span span,
                               std::string detail)
    : std::runtime_error(std::format("{}:{}:{}-{}:{}: {}: {}", filename, span.start_line,
                                     span.start_col, span.end_line, span.end_col, code, detail)),
      filename(std::move(filename)), code(std::move(code)), span(span), detail(std::move(detail)) {
}

namespace {

// A resolved type descriptor, separate from the source's type-syntax nodes. Shared
// element descriptors are compiler metadata, not ownership of JAQR runtime pointers.
struct value_type {
    enum class form : std::uint8_t { PRIMITIVE, STRUCT, POINTER, ARRAY } shape = form::PRIMITIVE;
    primitive_type primitive = primitive_type::VOID;
    std::string name;
    std::shared_ptr<value_type> element;
    std::int32_t length = 0;

    explicit value_type(primitive_type type = primitive_type::VOID) : primitive(type) {
    }
    static value_type named(std::string name) {
        value_type type;
        type.shape = form::STRUCT;
        type.name = std::move(name);
        return type;
    }
    static value_type pointer(value_type pointee) {
        value_type type;
        type.shape = form::POINTER;
        type.element = std::make_shared<value_type>(std::move(pointee));
        return type;
    }
    static value_type array(value_type element, std::int32_t length) {
        value_type type;
        type.shape = form::ARRAY;
        type.element = std::make_shared<value_type>(std::move(element));
        type.length = length;
        return type;
    }
};

// The primitive field is meaningful only for the PRIMITIVE shape (it stays VOID for
// the others), so every primitive test must check the shape as well.
bool is_primitive(const value_type &type, primitive_type kind) {
    return type.shape == value_type::form::PRIMITIVE && type.primitive == kind;
}
bool is_void(const value_type &type) {
    return is_primitive(type, primitive_type::VOID);
}
// A primitive that can hold a value: everything primitive except void.
bool is_scalar(const value_type &type) {
    return type.shape == value_type::form::PRIMITIVE && type.primitive != primitive_type::VOID;
}
bool is_pointer(const value_type &type) {
    return type.shape == value_type::form::POINTER;
}
bool is_array(const value_type &type) {
    return type.shape == value_type::form::ARRAY;
}
bool is_struct(const value_type &type) {
    return type.shape == value_type::form::STRUCT;
}

bool same(const value_type &a, const value_type &b) {
    if (a.shape != b.shape)
        return false;
    switch (a.shape) {
    case value_type::form::PRIMITIVE:
        return a.primitive == b.primitive;
    case value_type::form::STRUCT:
        return a.name == b.name;
    case value_type::form::POINTER:
        return same(*a.element, *b.element);
    case value_type::form::ARRAY:
        return a.length == b.length && same(*a.element, *b.element);
    }
    return false;
}

std::string describe(const value_type &type) {
    switch (type.shape) {
    case value_type::form::PRIMITIVE:
        return std::string(spelling(type.primitive));
    case value_type::form::STRUCT:
        return type.name;
    case value_type::form::POINTER:
        return "->" + describe(*type.element);
    case value_type::form::ARRAY:
        return std::format("[{}]{}", type.length, describe(*type.element));
    }
    return "<invalid>";
}

bool numeric(const value_type &type) {
    return is_primitive(type, primitive_type::INT) || is_primitive(type, primitive_type::FLOAT);
}
bool truthy(const value_type &type) {
    return numeric(type) || is_primitive(type, primitive_type::BOOL);
}
// TODO: this lets any numeric value initialise or be assigned to any numeric storage, so
// float -> int narrowing (which truncates) is implicit while every other conversion needs
// `as`. Whether narrowing should require an explicit conversion is a language-design
// decision for the owner; tightening it would reject programs that compile today.
bool compatible(const value_type &expected, const value_type &actual) {
    return same(expected, actual) || (numeric(expected) && numeric(actual));
}

const field_decl *find_field(const struct_decl &structure, std::string_view name) {
    const auto found = std::find_if(structure.fields.begin(), structure.fields.end(),
                                    [&](const field_decl &field) { return field.name == name; });
    return found == structure.fields.end() ? nullptr : &*found;
}

// TODO: constant() and numeric_constant() duplicate the constant folding in
// src/optimizer/optimizer.cpp (which also folds comparisons and logic). A shared folding
// utility over the AST would keep the checker's notion of "constant" in step with what the
// optimiser emits; it needs a new file both passes can include.

// Folds in 64 bits so overflow is detected instead of wrapping; a result outside int is
// not a constant.
std::optional<std::int32_t> fold_int(binary_op operation, std::int32_t left, std::int32_t right) {
    std::int64_t value = 0;
    switch (operation) {
    case binary_op::ADD:
        value = std::int64_t{left} + right;
        break;
    case binary_op::SUBTRACT:
        value = std::int64_t{left} - right;
        break;
    case binary_op::MULTIPLY:
        value = std::int64_t{left} * right;
        break;
    case binary_op::DIVIDE:
        if (right == 0)
            return std::nullopt;
        value = left / std::int64_t{right};
        break;
    default:
        return std::nullopt;
    }
    if (value < std::numeric_limits<std::int32_t>::min() ||
        value > std::numeric_limits<std::int32_t>::max())
        return std::nullopt;
    return static_cast<std::int32_t>(value);
}

// Compile-time integer evaluation for type constraints such as array lengths.
// This validates the source; the optimiser separately decides what to emit.
std::optional<std::int32_t> constant(const expr &node) {
    if (const auto *number = dynamic_cast<const literal *>(&node)) {
        if (const auto *integer = std::get_if<std::int32_t>(&number->value))
            return *integer;
        return std::nullopt;
    }
    if (const auto *unary = dynamic_cast<const unary_expr *>(&node)) {
        if (unary->operation != unary_op::NEGATE)
            return std::nullopt;
        const auto value = constant(*unary->right);
        if (value && *value != std::numeric_limits<std::int32_t>::min())
            return -*value;
        return std::nullopt;
    }
    if (const auto *binary = dynamic_cast<const binary_expr *>(&node)) {
        const auto left = constant(*binary->left);
        const auto right = constant(*binary->right);
        if (left && right)
            return fold_int(binary->operation, *left, *right);
    }
    return std::nullopt;
}

// Only used to detect a constant zero divisor, so it needs no more precision than double.
std::optional<double> numeric_constant(const expr &node) {
    // Preserve integer division when a constant expression contains only integers.
    if (const auto integer = constant(node))
        return static_cast<double>(*integer);
    if (const auto *number = dynamic_cast<const literal *>(&node)) {
        if (const auto *floating = std::get_if<double>(&number->value))
            return *floating;
        return std::nullopt;
    }
    if (const auto *unary = dynamic_cast<const unary_expr *>(&node)) {
        if (unary->operation != unary_op::NEGATE)
            return std::nullopt;
        if (const auto operand = numeric_constant(*unary->right))
            return -*operand;
        return std::nullopt;
    }
    if (const auto *cast = dynamic_cast<const conversion_expr *>(&node)) {
        const auto *type = dynamic_cast<const primitive_type_syntax *>(cast->target_type.get());
        const auto operand = numeric_constant(*cast->value);
        if (!type || !operand || !std::isfinite(*operand))
            return std::nullopt;
        if (type->type == primitive_type::FLOAT)
            return operand;
        if (type->type == primitive_type::INT &&
            *operand >= std::numeric_limits<std::int32_t>::min() &&
            *operand <= std::numeric_limits<std::int32_t>::max())
            return std::trunc(*operand);
        return std::nullopt;
    }
    if (const auto *binary = dynamic_cast<const binary_expr *>(&node)) {
        const auto left = numeric_constant(*binary->left);
        const auto right = numeric_constant(*binary->right);
        if (!left || !right)
            return std::nullopt;
        switch (binary->operation) {
        case binary_op::ADD:
            return *left + *right;
        case binary_op::SUBTRACT:
            return *left - *right;
        case binary_op::MULTIPLY:
            return *left * *right;
        case binary_op::DIVIDE:
        case binary_op::FLOOR_DIVIDE:
            if (*right != 0)
                return *left / *right;
            return std::nullopt;
        default:
            return std::nullopt;
        }
    }
    return std::nullopt;
}

// A structural proof: every if branch needs to return, including an else branch.
// Loops are not assumed to execute, so a return inside a loop alone is insufficient.
bool guarantees_return(const block_stmt &body) {
    for (const stmt_ptr &statement : body.statements) {
        if (dynamic_cast<const return_stmt *>(statement.get()))
            return true;
        if (const auto *nested = dynamic_cast<const block_stmt *>(statement.get())) {
            if (guarantees_return(*nested))
                return true;
        }
        if (const auto *branch = dynamic_cast<const if_stmt *>(statement.get())) {
            bool all = branch->else_body && guarantees_return(*branch->else_body);
            for (const conditional_branch &part : branch->branches)
                all = all && guarantees_return(*part.body);
            if (all)
                return true;
        }
    }
    return false;
}

// Sets a variable for the lifetime of the guard and restores the previous value on every
// exit, including a diagnostic thrown mid-visit. Without this, an analysis that recovers
// from one error to report more would continue with stale context flags.
template <class T> class scoped_value {
    T &target;
    T saved;

  public:
    scoped_value(T &variable, std::type_identity_t<T> value)
        : target(variable), saved(std::exchange(variable, std::move(value))) {
    }
    scoped_value(const scoped_value &) = delete;
    scoped_value &operator=(const scoped_value &) = delete;
    ~scoped_value() {
        target = std::move(saved);
    }
};

template <class... Handlers> struct overloaded : Handlers... {
    using Handlers::operator()...;
};
template <class... Handlers> overloaded(Handlers...) -> overloaded<Handlers...>;

// A dependent false, so a static_assert fires only when its template is instantiated.
template <class> constexpr bool unhandled_alternative = false;

class checker final : public visitor {
    std::string filename;
    analysis_root analysed;
    std::vector<const variable_decl *> invariant_inputs;
    builtin_calls builtins;

    [[noreturn]] void fail(const char *code, const source_span &span,
                           const std::string &message) const {
        throw semantic_error(filename, code, span, message);
    }

    struct binding {
        value_type type;
        bool state = false;
        bool immutable = false;
        const variable_decl *declaration = nullptr;
    };
    // Type alone cannot decide whether assignment or an update is legal. Carry storage
    // identity properties through member access/subscripts as well as the value's type.
    struct expr_result {
        value_type type{};
        bool location = false;  // Names storage that can be assigned or have its address taken.
        bool state = false;     // That storage is (part of) a state variable.
        bool immutable = false; // That storage is imut.
        const function_decl *callable = nullptr;
        bool statement = false;        // Legal as an expression statement: an assignment or a call.
        bool receiver = false;         // A method bound to the object it was accessed through.
        bool receiver_storage = false; // Storage reached through `this`.

        // A computed value with no storage identity.
        static expr_result rvalue(value_type type) {
            return {.type = std::move(type)};
        }
        // An assignment or call, whose value is not storage.
        static expr_result effect(value_type type) {
            return {.type = std::move(type), .statement = true};
        }
        // A field or array element shares the storage properties of the object holding it.
        static expr_result part_of(const expr_result &object, value_type type) {
            return {.type = std::move(type),
                    .location = object.location,
                    .state = object.state,
                    .immutable = object.immutable,
                    .receiver_storage = object.receiver_storage};
        }
    };
    static bool is_mutable_state(const expr_result &value) {
        return value.location && value.state && !value.immutable;
    }
    struct method_call {
        const function_decl *method;
        const function_decl *caller;
        source_span source;
        bool location;
        bool state;
        bool immutable;
        bool through_receiver;
    };

    symbol_table<binding, const struct_decl *, const function_decl *> symbols;
    std::unordered_map<const function_decl *, bool> writes_receiver;
    std::vector<method_call> method_calls;
    std::optional<expr_result> result;
    std::optional<value_type> resolved_type;
    std::optional<value_type> expected_array;
    bool initializer_context = false;
    const function_decl *current_function = nullptr;
    const struct_decl *current_struct = nullptr;
    int main_count = 0;
    bool global_mode = false;
    bool in_step = false;
    bool in_reject = false;
    bool in_invariant = false;

    bool in_update_function() const {
        return current_function && current_function->kind == function_kind::UPDATE;
    }

    expr_result evaluate(const expr &node, std::optional<value_type> expected = std::nullopt,
                         bool initializer = false) {
        // Array literals need their enclosing expected type (including length). Restore
        // it after each child so sibling expressions do not inherit the wrong context.
        const scoped_value expected_scope(expected_array, std::move(expected));
        const scoped_value initializer_scope(initializer_context, initializer);
        result.reset();
        node.accept(*this);
        if (!result)
            fail("SEM025", node.source, "expression is not supported yet");
        return *result;
    }
    value_type resolve(const type_syntax &node) {
        resolved_type.reset();
        node.accept(*this);
        if (!resolved_type)
            fail("SEM025", node.source, "type is not supported yet");
        return *resolved_type;
    }
    void require(const value_type &expected, const value_type &actual, const source_span &span,
                 bool exact = false) {
        if (!(exact ? same(expected, actual) : compatible(expected, actual)))
            fail("SEM003", span,
                 std::format("expected {}, got {}", describe(expected), describe(actual)));
    }
    void declare(const std::string &name, binding symbol, const source_span &span) {
        if (!symbols.declare_variable(name, std::move(symbol)))
            fail("SEM002", span, "duplicate name '" + name + "'");
    }
    void arguments(const std::vector<parameter> &params, const arg_list &args, bool state_args,
                   const source_span &call) {
        if (params.size() != args.size())
            fail("SEM006", call, "wrong number of arguments");
        for (size_t i = 0; i < args.size(); ++i) {
            const value_type type = resolve(*params[i].declared_type);
            const expr_result actual = evaluate(*args[i], type);
            require(type, actual.type, args[i]->source, true);
            if (state_args && !is_mutable_state(actual))
                fail("SEM018", args[i]->source, "state argument must be mutable state storage");
        }
    }
    expr_result string_literal(const literal &node, const std::string &text) const {
        if (text.size() > static_cast<size_t>(std::numeric_limits<std::int32_t>::max()))
            fail("SEM013", node.source, "string literal is too long");
        const value_type chars{primitive_type::CHAR};
        // A string initializing a char array takes the array's (possibly larger) type.
        if (initializer_context && expected_array && is_array(*expected_array) &&
            same(*expected_array->element, chars)) {
            if (text.size() > static_cast<size_t>(expected_array->length))
                fail("SEM013", node.source, "string has more characters than the array holds");
            return expr_result::rvalue(*expected_array);
        }
        if (text.empty())
            fail("SEM013", node.source, "empty string requires a char-array initializer");
        return expr_result::rvalue(
            value_type::array(chars, static_cast<std::int32_t>(text.size())));
    }
    // The grammar accepts main only as `fn main()` at file scope, and a second one fails
    // declare_function first, so these guard ASTs built by hand rather than parsed.
    void check_main(const function_decl &node) {
        if (current_struct)
            fail("SEM002", node.source, "a method cannot be named main");
        if (node.kind != function_kind::ORDINARY)
            fail("SEM002", node.source, "main cannot be an update function");
        if (!node.ordinary_parameters.empty())
            fail("SEM002", node.source, "main cannot take parameters");
        if (++main_count != 1)
            fail("SEM002", node.source, "only one main function is allowed");
    }

  public:
    explicit checker(std::string filename) : filename(std::move(filename)) {
    }

    analysis_root run(program_ptr ast) {
        analysed.ast = std::move(ast);
        const program &root = *analysed.ast;
        try {
            builtins = resolve_builtins(root);
        } catch (const builtin_error &error) {
            // The resolver has no filename; attach it so I/O diagnostics match the others.
            fail(error.code.c_str(), error.span, error.detail);
        }
        {
            const scoped_value global_scope(global_mode, true);
            for (const decl_ptr &entry : root.declarations)
                entry->accept(*this);
        }
        if (main_count != 1)
            fail("SEM024",
                 root.declarations.empty() ? source_span{0, 0, 0, 0}
                                           : root.declarations.back()->source,
                 "exactly one fn main() is required");

        // Method calls on this can transmit receiver writes through recursive call chains.
        // Wait until all bodies are checked, then propagate until no effect changes;
        // a single pass would depend on method order and miss indirect writes.
        bool changed;
        do {
            changed = false;
            for (const method_call &call : method_calls) {
                if (call.caller && call.through_receiver && writes_receiver[call.method] &&
                    !writes_receiver[call.caller]) {
                    writes_receiver[call.caller] = true;
                    changed = true;
                }
            }
        } while (changed);
        for (const method_call &call : method_calls) {
            if (!writes_receiver[call.method])
                continue;
            if (!call.location || call.immutable)
                fail("SEM012", call.source, "writing method requires a mutable receiver");
            if (call.state)
                fail("SEM017", call.source, "ordinary method cannot modify state storage");
        }
        return std::move(analysed);
    }

    void visit(const program &) override {
    }

    void visit(const literal &node) override {
        // Every literal_val alternative needs a handler: the catch-all template turns a new
        // alternative into a compile error instead of an implicit conversion or SEM025.
        result = std::visit(
            overloaded{
                [](std::int32_t) { return expr_result::rvalue(value_type{primitive_type::INT}); },
                [](double) { return expr_result::rvalue(value_type{primitive_type::FLOAT}); },
                [](char) { return expr_result::rvalue(value_type{primitive_type::CHAR}); },
                [](std::byte) { return expr_result::rvalue(value_type{primitive_type::BYTE}); },
                [](bool) { return expr_result::rvalue(value_type{primitive_type::BOOL}); },
                [&](const std::string &text) { return string_literal(node, text); },
                [](const auto &unhandled) {
                    static_assert(unhandled_alternative<decltype(unhandled)>,
                                  "every literal kind needs a type");
                }},
            node.value);
    }
    void visit(const identifier &node) override {
        if (const binding *symbol = symbols.lookup_variable(node.lexeme)) {
            if (in_invariant) {
                if (!symbol->state)
                    fail("SEM020", node.source, "invariants may read only state variables");
                if (!symbol->declaration)
                    fail("SEM020", node.source, "invariants cannot use state parameters yet");
                if (std::find(invariant_inputs.begin(), invariant_inputs.end(),
                              symbol->declaration) == invariant_inputs.end())
                    invariant_inputs.push_back(symbol->declaration);
            }
            result = expr_result{.type = symbol->type,
                                 .location = true,
                                 .state = symbol->state,
                                 .immutable = symbol->immutable};
        } else if (const function_decl *fn = symbols.lookup_function(node.lexeme)) {
            if (in_invariant)
                fail("SEM020", node.source, "invariants cannot reference functions");
            result = expr_result{.callable = fn};
        } else
            fail("SEM001", node.source, "unknown name '" + node.lexeme + "'");
    }
    void visit(const conversion_expr &node) override {
        const value_type target = resolve(*node.target_type);
        if (!is_scalar(target))
            fail("SEM004", node.source, "invalid conversion target");
        const value_type source = evaluate(*node.value).type;
        if (!is_scalar(source))
            fail("SEM004", node.source, "invalid conversion source");
        result = expr_result::rvalue(target);
    }
    void visit(const unary_expr &node) override {
        if (in_invariant &&
            (node.operation == unary_op::DEREFERENCE || node.operation == unary_op::ADDRESS))
            fail("SEM020", node.source, "invariants cannot use pointer operators");
        const expr_result operand = evaluate(*node.right);
        switch (node.operation) {
        case unary_op::LOGICAL_NOT:
            if (truthy(operand.type)) {
                result = expr_result::rvalue(value_type{primitive_type::BOOL});
                return;
            }
            break;
        case unary_op::NEGATE:
            if (numeric(operand.type)) {
                result = expr_result::rvalue(operand.type);
                return;
            }
            break;
        case unary_op::ADDRESS:
            // A pointer could later write through storage the language protects.
            if (operand.location && !operand.immutable && !operand.state &&
                !operand.receiver_storage) {
                result = expr_result::rvalue(value_type::pointer(operand.type));
                return;
            }
            fail("SEM016", node.source, "address-of requires mutable non-state storage");
        case unary_op::DEREFERENCE:
            if (is_pointer(operand.type)) {
                result = expr_result{.type = *operand.type.element, .location = true};
                return;
            }
            fail("SEM015", node.source, "dereference requires a pointer");
        }
        fail("SEM004", node.source, "invalid unary operand");
    }
    void visit(const binary_expr &node) override {
        const value_type left = evaluate(*node.left).type;
        const value_type right = evaluate(*node.right).type;
        const bool both_numeric = numeric(left) && numeric(right);
        const value_type boolean{primitive_type::BOOL};
        switch (node.operation) {
        case binary_op::ADD:
        case binary_op::SUBTRACT:
            if (node.operation == binary_op::SUBTRACT && is_pointer(left) && same(left, right)) {
                result = expr_result::rvalue(value_type{primitive_type::INT});
                return;
            }
            if (is_pointer(left) && is_primitive(right, primitive_type::INT)) {
                result = expr_result::rvalue(left);
                return;
            }
            if (node.operation == binary_op::ADD && is_primitive(left, primitive_type::INT) &&
                is_pointer(right)) {
                result = expr_result::rvalue(right);
                return;
            }
            [[fallthrough]];
        case binary_op::MULTIPLY:
        case binary_op::DIVIDE:
        case binary_op::FLOOR_DIVIDE:
        case binary_op::EXPONENT:
            if (both_numeric) {
                if ((node.operation == binary_op::DIVIDE ||
                     node.operation == binary_op::FLOOR_DIVIDE) &&
                    numeric_constant(*node.right) == 0.0)
                    fail("SEM005", node.right->source, "division by constant zero");
                const bool real = is_primitive(left, primitive_type::FLOAT) ||
                                  is_primitive(right, primitive_type::FLOAT);
                result = expr_result::rvalue(
                    value_type{real ? primitive_type::FLOAT : primitive_type::INT});
                return;
            }
            break;
        case binary_op::BITWISE_AND:
        case binary_op::BITWISE_OR:
        case binary_op::BITWISE_XOR:
        case binary_op::LEFT_SHIFT:
        case binary_op::RIGHT_SHIFT:
            if (is_primitive(left, primitive_type::INT) &&
                is_primitive(right, primitive_type::INT)) {
                if (node.operation == binary_op::LEFT_SHIFT ||
                    node.operation == binary_op::RIGHT_SHIFT) {
                    const auto shift = constant(*node.right);
                    if (shift && (*shift < 0 || *shift >= 32))
                        fail("SEM004", node.right->source, "shift count must be between 0 and 31");
                }
                result = expr_result::rvalue(value_type{primitive_type::INT});
                return;
            }
            break;
        case binary_op::LOGICAL_AND:
        case binary_op::LOGICAL_OR:
            if (truthy(left) && truthy(right)) {
                result = expr_result::rvalue(boolean);
                return;
            }
            break;
        case binary_op::EQUALS:
        case binary_op::NOT_EQUALS:
            if (both_numeric || (same(left, right) && (is_pointer(left) || is_scalar(left)))) {
                result = expr_result::rvalue(boolean);
                return;
            }
            break;
        case binary_op::GT:
        case binary_op::GTE:
        case binary_op::LT:
        case binary_op::LTE:
            if (both_numeric || (same(left, right) && (is_primitive(left, primitive_type::CHAR) ||
                                                       is_primitive(left, primitive_type::BYTE)))) {
                result = expr_result::rvalue(boolean);
                return;
            }
            break;
        }
        if (is_pointer(left) || is_pointer(right))
            fail("SEM015", node.source, "invalid pointer operation");
        fail("SEM004", node.source, "invalid binary operand types");
    }
    void visit(const assignment_expr &node) override {
        if (in_invariant)
            fail("SEM020", node.source, "invariants cannot assign to storage");
        const expr_result target = evaluate(*node.target);
        if (!target.location)
            fail("SEM004", node.target->source, "assignment target is not storage");
        if (target.immutable)
            fail("SEM012", node.target->source, "cannot assign to immutable storage");
        // State changes only where a failed step can roll them back: an update function,
        // or a step written directly in main (not in a helper a step happens to call).
        // A reject branch runs after the rollback, so it may not write state.
        const bool step_in_main =
            in_step && (!current_function || current_function->name->lexeme == "main");
        const bool may_write_state = !in_reject && (in_update_function() || step_in_main);
        if (target.state && !may_write_state)
            fail("SEM017", node.target->source, "state write outside a step or update context");
        if (target.receiver_storage && current_function)
            writes_receiver[current_function] = true;
        const expr_result value = evaluate(*node.value, target.type);
        require(target.type, value.type, node.value->source);
        result = expr_result::effect(target.type);
    }
    void visit(const array_literal &node) override {
        std::optional<value_type> target = expected_array;
        if (!target && !node.elements.empty())
            target = value_type::array(evaluate(*node.elements.front()).type,
                                       static_cast<std::int32_t>(node.elements.size()));
        if (!target || !is_array(*target))
            fail("SEM013", node.source, "array literal needs an array type");
        if (node.elements.size() > static_cast<size_t>(target->length))
            fail("SEM013", node.source, "too many array elements");
        for (const expr_ptr &element : node.elements)
            require(*target->element, evaluate(*element, *target->element, true).type,
                    element->source);
        result = expr_result::rvalue(*target);
    }
    void visit(const struct_literal &node) override {
        const struct_decl *structure = symbols.lookup_struct(node.struct_type);
        if (!structure)
            fail("SEM001", node.source, "unknown struct '" + node.struct_type + "'");
        std::unordered_set<std::string> seen;
        for (const field_init &field : node.init_list) {
            const field_decl *member = find_field(*structure, field.name);
            if (!member)
                fail("SEM004", field.val->source,
                     "struct '" + node.struct_type + "' has no field '" + field.name + "'");
            if (!seen.insert(field.name).second)
                fail("SEM004", field.val->source,
                     "struct field '" + field.name + "' is initialised more than once");
            const value_type type = resolve(*member->declared_type);
            require(type, evaluate(*field.val, type, true).type, field.val->source);
        }
        result = expr_result::rvalue(value_type::named(node.struct_type));
    }
    void visit(const dynamic_alloc &node) override {
        if (in_invariant)
            fail("SEM020", node.source, "invariants cannot allocate storage");
        const value_type element = resolve(*node.element_type);
        const value_type count = evaluate(*node.count).type;
        require(value_type{primitive_type::INT}, count, node.count->source, true);
        result = expr_result::rvalue(value_type::pointer(element));
    }
    void visit(const this_expr &node) override {
        if (in_invariant)
            fail("SEM020", node.source, "invariants cannot reference this");
        if (!current_struct)
            fail("SEM001", node.source, "this is only valid in a method");
        // The receiver is state storage only inside an update method.
        result = expr_result{.type = value_type::named(current_struct->name->lexeme),
                             .location = true,
                             .state = in_update_function(),
                             .receiver_storage = true};
    }
    void visit(const postfix_expr &node) override {
        if (in_invariant &&
            (node.op == postfix_op::FUNCT_CALL || node.op == postfix_op::UPDATE_CALL))
            fail("SEM020", node.source, "invariants cannot call functions");
        if (const auto builtin = builtins.find(&node); builtin != builtins.end()) {
            // resolve_builtins has already checked the argument count and its type.
            const auto &args = std::get<arg_list>(node.arg);
            evaluate(*args.front());
            result = expr_result::effect(value_type{builtin->second == builtin_function::scan
                                                        ? primitive_type::BOOL
                                                        : primitive_type::VOID});
            return;
        }
        const expr_result object = evaluate(*node.obj);
        switch (node.op) {
        case postfix_op::SUBSCRIPT: {
            const bool pointer = is_pointer(object.type);
            if (in_invariant && pointer)
                fail("SEM020", node.source, "invariants cannot dereference pointers");
            const value_type index = evaluate(*std::get<expr_ptr>(node.arg)).type;
            require(value_type{primitive_type::INT}, index, node.source, true);
            if (!is_array(object.type) && !pointer)
                fail("SEM014", node.source, "indexing requires an array or pointer");
            // An array element is part of the array; a pointer's target is separate storage
            // whose state or immutability the pointer type does not record.
            result = pointer ? expr_result{.type = *object.type.element, .location = true}
                             : expr_result::part_of(object, *object.type.element);
            return;
        }
        case postfix_op::MEMBER_ACCESS: {
            if (!is_struct(object.type))
                fail("SEM004", node.source, "member access requires a struct");
            const std::string &member = std::get<std::string>(node.arg);
            if (const function_decl *method = symbols.lookup_method(object.type.name, member)) {
                result = expr_result{.location = object.location,
                                     .state = object.state,
                                     .immutable = object.immutable,
                                     .callable = method,
                                     .receiver = true,
                                     .receiver_storage = object.receiver_storage};
                return;
            }
            const struct_decl *structure = symbols.lookup_struct(object.type.name);
            if (!structure)
                fail("SEM001", node.source, "unknown struct type");
            const field_decl *field = find_field(*structure, member);
            if (!field)
                fail("SEM001", node.source, "unknown field '" + member + "'");
            result = expr_result::part_of(object, resolve(*field->declared_type));
            return;
        }
        case postfix_op::FUNCT_CALL:
        case postfix_op::UPDATE_CALL: {
            const function_decl *fn = object.callable;
            if (!fn)
                fail("SEM006", node.source, "expression is not callable");
            const bool update = fn->kind == function_kind::UPDATE;
            if (update && object.receiver && !is_mutable_state(object))
                fail("SEM018", node.source, "update method requires a mutable state receiver");
            if (!update && object.receiver)
                method_calls.push_back(method_call{fn, current_function, node.source,
                                                   object.location, object.state, object.immutable,
                                                   object.receiver_storage});
            if (node.op == postfix_op::UPDATE_CALL && !update)
                fail("SEM018", node.source, "two argument clauses require an update function");
            // An update runs inside a step, directly or from another update function, so a
            // failed step can roll it back. A reject branch runs after that rollback.
            const bool may_call_update = !in_reject && (in_step || in_update_function());
            if (update && !may_call_update)
                fail("SEM018", node.source, "update call requires an active step");
            if (node.op == postfix_op::UPDATE_CALL) {
                const auto &args = std::get<update_arguments>(node.arg);
                arguments(fn->state_parameters, args.state, true, node.source);
                arguments(fn->ordinary_parameters, args.regular, false, node.source);
            } else {
                if (update && !fn->state_parameters.empty())
                    fail("SEM018", node.source, "update function requires state arguments");
                arguments(fn->ordinary_parameters, std::get<arg_list>(node.arg), false,
                          node.source);
            }
            result = expr_result::effect(fn->return_type ? resolve(*fn->return_type)
                                                         : value_type{primitive_type::VOID});
            return;
        }
        }
    }

    void visit(const primitive_type_syntax &node) override {
        resolved_type = value_type{node.type};
    }
    void visit(const named_type_syntax &node) override {
        if (!symbols.lookup_struct(node.name))
            fail("SEM001", node.source, "unknown type '" + node.name + "'");
        resolved_type = value_type::named(node.name);
    }
    void visit(const pointer_type_syntax &node) override {
        resolved_type = value_type::pointer(resolve(*node.pointee));
    }
    void visit(const array_type_syntax &node) override {
        const auto length = constant(*node.size);
        if (!length || *length <= 0)
            fail("SEM013", node.size->source, "array size must be a positive constant");
        resolved_type = value_type::array(resolve(*node.element), *length);
    }

    void visit(const variable_decl &node) override {
        if (in_reject && node.storage == variable_storage::STATE)
            fail("SEM017", node.source, "reject branch cannot declare state");
        const value_type type = resolve(*node.declared_type);
        if (is_void(type))
            fail("SEM004", node.source, "void is not a variable type");
        if ((node.immutable || node.storage == variable_storage::STATE) && !node.initializer)
            fail("SEM011", node.source, "state and immutable variables need an initializer");
        if (node.initializer)
            require(type, evaluate(*node.initializer, type, true).type, node.initializer->source);
        declare(node.name->lexeme,
                binding{type, node.storage == variable_storage::STATE, node.immutable, &node},
                node.name->source);
    }
    // TODO: field_decl has no source span, so a duplicate field is reported at the whole
    // struct. Adding a span to field_decl (src/ast/declaration.h, filled in by the parser)
    // would let the diagnostic point at the repeated field.
    void visit(const struct_decl &node) override {
        if (!global_mode)
            fail("SEM002", node.source, "structs must be declared at file scope");
        if (!symbols.declare_struct(node.name->lexeme, &node))
            fail("SEM002", node.source, "duplicate name '" + node.name->lexeme + "'");
        std::unordered_set<std::string> names;
        for (const field_decl &field : node.fields) {
            if (!names.insert(field.name).second)
                fail("SEM002", node.source, "duplicate field '" + field.name + "'");
            resolve(*field.declared_type);
        }
    }
    void visit(const impl_decl &node) override {
        // Register every signature before checking any body so methods within this impl
        // can call one another, including recursively, regardless of their order.
        const struct_decl *structure = symbols.lookup_struct(node.type_name->lexeme);
        if (!structure)
            fail("SEM001", node.source, "unknown impl type");
        for (const function_decl_ptr &method : node.methods) {
            if (!symbols.declare_method(node.type_name->lexeme, method->name->lexeme, method.get()))
                fail("SEM002", method->source, "duplicate method");
            analysed.impls[node.type_name->lexeme].push_back(method.get());
        }
        const scoped_value struct_scope(current_struct, structure);
        for (const function_decl_ptr &method : node.methods)
            method->accept(*this);
    }
    // TODO: a free function is declared only when its definition is reached, so calls
    // before the definition and mutual recursion report SEM001 (the reject tests
    // call_before_definition and mutual_recursion pin this down, and the spec documents
    // it). Allowing them would take a first pass over the file-scope declarations that
    // registers every signature before any body is checked, as visit(impl_decl) already
    // does for methods; resolve_builtins would need the same treatment.
    //
    // TODO: parameter has no source span, so a bad parameter is reported at the whole
    // function. Adding a span to parameter (src/ast/declaration.h, filled in by the
    // parser's parse_parameter_list) would let the diagnostic point at the parameter.
    void visit(const function_decl &node) override {
        if (!current_struct) {
            if (!global_mode)
                fail("SEM002", node.source, "functions must be declared at file scope");
            if (!symbols.declare_function(node.name->lexeme, &node))
                fail("SEM002", node.source, "duplicate function '" + node.name->lexeme + "'");
        }
        if (node.name->lexeme == "main")
            check_main(node);
        auto function_scope = symbols.enter_scope();
        const scoped_value function_context(current_function, &node);
        const scoped_value local_context(global_mode, false);
        std::unordered_set<std::string> parameter_names;
        const auto declare_parameter = [&](const parameter &param, bool state) {
            // Resolve first: an unknown parameter type (SEM001) outranks a duplicate name.
            value_type type = resolve(*param.declared_type);
            if (!parameter_names.insert(param.name).second)
                fail("SEM002", node.source,
                     "duplicate parameter '" + param.name + "' in function '" + node.name->lexeme +
                         "'");
            declare(param.name, binding{std::move(type), state}, node.source);
        };
        for (const parameter &param : node.state_parameters)
            declare_parameter(param, true);
        for (const parameter &param : node.ordinary_parameters)
            declare_parameter(param, false);
        node.body->accept(*this);
        if (node.return_type && !is_void(resolve(*node.return_type)) &&
            !guarantees_return(*node.body))
            fail("SEM008", node.source, "function has a path without a return value");
    }
    void visit(const invariant_decl &node) override {
        invariant_inputs.clear();
        {
            const scoped_value invariant_scope(in_invariant, true);
            const value_type type = evaluate(*node.condition).type;
            if (!is_primitive(type, primitive_type::BOOL))
                fail("SEM020", node.condition->source, "invariant condition must have type bool");
        }
        // The parsed AST is owned by analysed.ast, so its codegen metadata is mutable.
        // TODO: this writes to the AST through a const visitor. Cleaner designs: record
        // the inputs in a side table in analysis_root keyed by the invariant_decl (the
        // code generator would read it instead of invariant_decl::state_vars, so this
        // needs changes to semantic.h, src/ast/declaration.h, main.cpp and the code
        // generator), or run analysis as a non-const pass that owns the tree.
        const_cast<invariant_decl &>(node).state_vars = invariant_inputs;
        for (const variable_decl *input : invariant_inputs)
            analysed.invs[input->name->lexeme].push_back(&node);
    }
    void visit(const block_stmt &node) override {
        auto block_scope = symbols.enter_scope();
        for (const stmt_ptr &stmt : node.statements)
            stmt->accept(*this);
    }
    void visit(const decl_stmt &node) override {
        node.declaration->accept(*this);
    }
    void visit(const expr_stmt &node) override {
        if (!evaluate(*node.expression).statement)
            fail("SEM004", node.source, "statement must be an assignment or call");
    }
    void visit(const return_stmt &node) override {
        if (!current_function || in_step || in_reject)
            fail("SEM023", node.source, "return is not allowed here");
        // The parser gives main an explicit void return type, so only update functions
        // (and hand-built ASTs) reach this without one.
        if (!current_function->return_type) {
            if (node.value)
                fail("SEM007", node.source,
                     current_function->kind == function_kind::UPDATE
                         ? "update function cannot return a value"
                         : "function without a return type cannot return a value");
            return;
        }
        const value_type type = resolve(*current_function->return_type);
        if (node.value)
            require(type, evaluate(*node.value, type).type, node.value->source, true);
        else if (!is_void(type))
            fail("SEM007", node.source, "return value required");
    }
    void visit(const if_stmt &node) override {
        for (const conditional_branch &branch : node.branches) {
            if (!truthy(evaluate(*branch.condition).type))
                fail("SEM004", branch.condition->source, "condition must be bool or numeric");
            branch.body->accept(*this);
        }
        if (node.else_body)
            node.else_body->accept(*this);
    }
    void visit(const while_stmt &node) override {
        if (!truthy(evaluate(*node.condition).type))
            fail("SEM004", node.condition->source, "condition must be bool or numeric");
        node.body->accept(*this);
    }
    void visit(const for_stmt &node) override {
        auto loop_scope = symbols.enter_scope();
        node.initializer->accept(*this);
        if (!truthy(evaluate(*node.condition).type))
            fail("SEM004", node.condition->source, "condition must be bool or numeric");
        evaluate(*node.increment);
        node.body->accept(*this);
    }
    void visit(const step_stmt &node) override {
        if (in_step || in_reject)
            fail("SEM019", node.source,
                 "step cannot be nested in another step or its reject branch");
        if (in_update_function())
            fail("SEM019", node.source, "update function cannot contain a step");
        {
            const scoped_value step_scope(in_step, true);
            node.body->accept(*this);
        }
        if (node.reject_body) {
            const scoped_value reject_scope(in_reject, true);
            node.reject_body->accept(*this);
        }
    }
};

} // namespace

analysis_root analyse(program_ptr root, std::string_view filename) {
    return checker{std::string(filename)}.run(std::move(root));
}
