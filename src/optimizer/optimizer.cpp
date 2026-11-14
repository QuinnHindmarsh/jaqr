#include "optimizer.h"

#include "../visitor/visitor.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

// TODO: integer constant evaluation here duplicates constant()/numeric_constant() and fold_int()
// in src/semantic/semantic.cpp, and the two already disagree (semantic folds DIVIDE, this pass
// does not). Move both onto one shared constant-evaluation module (e.g.
// src/constant/constant_eval.h over literal_val) so the checker's notion of a constant and what
// the optimiser emits cannot drift apart.

namespace {

// JAQR int is 32 bits. Arithmetic is done in 64 bits so an out-of-range result is detected
// instead of wrapping; such a result stays in the emitted program to be evaluated at runtime.
std::optional<std::int32_t> as_int32(std::int64_t value) {
    if (value < std::numeric_limits<std::int32_t>::min() ||
        value > std::numeric_limits<std::int32_t>::max())
        return std::nullopt;
    return static_cast<std::int32_t>(value);
}

// The result of comparing two integers, or nullopt if op is not a comparison.
std::optional<bool> compare(binary_op op, std::int64_t lhs, std::int64_t rhs) {
    switch (op) {
    case binary_op::EQUALS:
        return lhs == rhs;
    case binary_op::NOT_EQUALS:
        return lhs != rhs;
    case binary_op::GT:
        return lhs > rhs;
    case binary_op::GTE:
        return lhs >= rhs;
    case binary_op::LT:
        return lhs < rhs;
    case binary_op::LTE:
        return lhs <= rhs;
    default:
        return std::nullopt;
    }
}

bool is_ordering(binary_op op) {
    return op == binary_op::LT || op == binary_op::LTE || op == binary_op::GT ||
           op == binary_op::GTE;
}

std::optional<literal_val> fold_integers(binary_op op, std::int64_t lhs, std::int64_t rhs) {
    std::optional<std::int32_t> result;
    switch (op) {
    case binary_op::ADD:
        result = as_int32(lhs + rhs);
        break;
    case binary_op::SUBTRACT:
        result = as_int32(lhs - rhs);
        break;
    case binary_op::MULTIPLY:
        result = as_int32(lhs * rhs);
        break;
    default:
        if (const auto comparison = compare(op, lhs, rhs))
            return *comparison;
        // TODO: division, floor division, exponent, shifts and bitwise operators are not folded,
        // nor is any floating-point arithmetic. Division and shifts need the runtime's exact
        // semantics for zero divisors, negative operands and oversized shift counts first.
        return std::nullopt;
    }
    if (result)
        return *result;
    return std::nullopt;
}

// A known left operand may make the right operand unreachable through short-circuiting.
// A known right operand alone cannot justify dropping the left one.
std::optional<bool> fold_booleans(binary_op op, std::optional<bool> lhs, std::optional<bool> rhs) {
    if (!lhs)
        return std::nullopt;
    switch (op) {
    case binary_op::LOGICAL_AND:
        if (!*lhs)
            return false;
        return rhs;
    case binary_op::LOGICAL_OR:
        if (*lhs)
            return true;
        return rhs;
    case binary_op::EQUALS:
        if (rhs)
            return *lhs == *rhs;
        return std::nullopt;
    case binary_op::NOT_EQUALS:
        if (rhs)
            return *lhs != *rhs;
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

template <typename... Handlers> struct overloaded : Handlers... {
    using Handlers::operator()...;
};

// A for loop of the form `for (let i: int = initial; i <comparison> bound; i = i +/- step)`,
// with initial, bound and step all known integers.
struct counted_loop {
    const std::string *counter;
    std::int64_t initial;
    std::int64_t bound;
    binary_op comparison; // LT, LTE, GT or GTE
    std::int64_t delta;   // never zero
};

// A statement `name = value` where evaluating value only reads variables (see reads()).
struct simple_assignment {
    const std::string *target;
    const expr *value;
};

// A statement of a block that dead_locals() can reason about. Classifying every statement up
// front means the liveness walk never has to re-inspect or cast the AST.
struct simple_statement {
    enum class kind : std::uint8_t { DECLARATION, ASSIGNMENT, RETURN };

    kind what;
    const stmt *node;
    const std::string *name; // the declared or assigned variable; null only for a return
    const expr *value;       // initializer, assigned or returned value; may be null
};

struct simple_block {
    std::vector<simple_statement> statements;
    // Mutable, ordinary int locals declared by the block: the only removal candidates.
    std::unordered_set<std::string> candidates;
};

// Traversal is a visitor so that adding an AST node without teaching the optimiser about it is
// a compile error rather than a node silently left unoptimised. The pattern matchers below
// (reads(), the counted-loop and simple-block recognisers) deliberately stay dynamic_cast
// whitelists instead: anything they do not recognise, including a future node, makes them
// decline to optimise, which is the safe answer.
class optimizer final : public visitor {
    optimization_options options;
    optimization_plan plan;

    // Set by a statement's visit when control cannot continue past that statement, as its last
    // action. statement() reads and clears it, so it is false whenever control returns into an
    // enclosing visit.
    bool terminates = false;

    const literal_val *constant(const expr *node) const {
        const auto found = plan.constants.find(node);
        return found == plan.constants.end() ? nullptr : &found->second;
    }
    std::optional<std::int32_t> integer(const expr *node) const {
        if (const auto *value = constant(node)) {
            if (const auto *number = std::get_if<std::int32_t>(value))
                return *number;
        }
        return std::nullopt;
    }
    // Every expression is visited exactly once, so each node gets at most one value.
    void record(const expr &node, literal_val value) {
        [[maybe_unused]] const bool inserted =
            plan.constants.emplace(&node, std::move(value)).second;
        assert(inserted);
    }

    void expression(const expr *node) {
        if (node)
            node->accept(*this);
    }
    bool statement(const stmt &node) {
        node.accept(*this);
        return std::exchange(terminates, false);
    }
    void declaration(const decl &node) {
        node.accept(*this);
    }

    // Returns whether the block ends control flow: a statement after one that does is omitted.
    bool block(const block_stmt &node) {
        bool terminated = false;
        for (const auto &item : node.statements) {
            if (terminated)
                plan.omitted.insert(item.get());
            else
                terminated = statement(*item);
        }
        dead_locals(node);
        return terminated;
    }

    std::optional<literal_val> fold(const binary_expr &node) const {
        const auto lhs = integer(node.left.get());
        const auto rhs = integer(node.right.get());
        if (lhs && rhs)
            return fold_integers(node.operation, *lhs, *rhs);
        if (const auto result = fold_booleans(node.operation, plan.boolean(node.left.get()),
                                              plan.boolean(node.right.get())))
            return *result;
        return std::nullopt;
    }

    // True if evaluating node only reads named variables: no calls, aliasing, writes or hidden
    // state access. Adds the names read to names; on false, names may be partly filled.
    bool reads(const expr *node, std::unordered_set<std::string> &names) const {
        if (!node || constant(node))
            return true;
        if (const auto *name = dynamic_cast<const identifier *>(node)) {
            names.insert(name->lexeme);
            return true;
        }
        if (const auto *binary = dynamic_cast<const binary_expr *>(node))
            return reads(binary->left.get(), names) && reads(binary->right.get(), names);
        if (const auto *unary = dynamic_cast<const unary_expr *>(node))
            return (unary->operation == unary_op::NEGATE ||
                    unary->operation == unary_op::LOGICAL_NOT) &&
                   reads(unary->right.get(), names);
        return false;
    }
    bool only_reads(const expr *node) const {
        std::unordered_set<std::string> ignored;
        return reads(node, ignored);
    }

    std::optional<simple_assignment> as_simple_assignment(const stmt *node) const {
        const auto *item = dynamic_cast<const expr_stmt *>(node);
        if (!item)
            return std::nullopt;
        const auto *assignment = dynamic_cast<const assignment_expr *>(item->expression.get());
        if (!assignment)
            return std::nullopt;
        const auto *target = dynamic_cast<const identifier *>(assignment->target.get());
        if (!target || !only_reads(assignment->value.get()))
            return std::nullopt;
        return simple_assignment{&target->lexeme, assignment->value.get()};
    }

    // ---- loop unrolling ----
    // Prove the trip count only for a bounded integer-counter pattern. Calls, aliasing writes
    // and body changes to the counter prevent that proof, so those loops are left alone.

    std::optional<counted_loop> match_counted_loop(const for_stmt &loop) const {
        const variable_decl *init = loop.initializer.get();
        if (!init || init->immutable || init->storage != variable_storage::ORDINARY)
            return std::nullopt;
        const auto *type = dynamic_cast<const primitive_type_syntax *>(init->declared_type.get());
        if (!type || type->type != primitive_type::INT)
            return std::nullopt;
        const std::string &counter = init->name->lexeme;
        const auto is_counter = [&](const expr *node) {
            const auto *id = dynamic_cast<const identifier *>(node);
            return id && id->lexeme == counter;
        };

        const auto initial = integer(init->initializer.get());
        const auto *test = dynamic_cast<const binary_expr *>(loop.condition.get());
        if (!initial || !test || !is_ordering(test->operation) || !is_counter(test->left.get()))
            return std::nullopt;
        const auto bound = integer(test->right.get());

        const auto *increment = dynamic_cast<const assignment_expr *>(loop.increment.get());
        if (!bound || !increment || !is_counter(increment->target.get()))
            return std::nullopt;
        const auto *next = dynamic_cast<const binary_expr *>(increment->value.get());
        if (!next || !is_counter(next->left.get()) ||
            (next->operation != binary_op::ADD && next->operation != binary_op::SUBTRACT))
            return std::nullopt;
        const auto amount = integer(next->right.get());
        if (!amount || *amount == 0)
            return std::nullopt;

        const std::int64_t delta =
            next->operation == binary_op::ADD ? *amount : -static_cast<std::int64_t>(*amount);
        return counted_loop{&counter, *initial, *bound, test->operation, delta};
    }

    // Unrolling copies the body, so it may only hold simple assignments that leave the counter
    // alone. This conservatively checks statements already omitted from the body as well.
    bool unrollable_body(const block_stmt &body, const std::string &counter) const {
        for (const auto &statement : body.statements) {
            const auto assignment = as_simple_assignment(statement.get());
            if (!assignment || *assignment->target == counter)
                return false;
        }
        return true;
    }

    // Simulates the counter. Both the iteration and statement budgets bound compile time and
    // generated code size; nullopt if either is exceeded or the counter would overflow, which
    // includes the final increment that makes the condition false.
    std::optional<std::size_t> trip_count(const counted_loop &loop,
                                          std::size_t body_statements) const {
        // Each unrolled iteration emits the body statements and then the increment.
        const std::size_t per_iteration = body_statements + 1;
        std::int64_t value = loop.initial;
        std::size_t count = 0;
        // compare() always has a value here because the comparison is an ordering.
        while (compare(loop.comparison, value, loop.bound).value_or(false)) {
            ++count;
            if (count > options.max_unroll_iterations ||
                count > options.max_unrolled_statements / per_iteration)
                return std::nullopt;
            value += loop.delta;
            if (!as_int32(value))
                return std::nullopt;
        }
        return count;
    }

    void unroll(const for_stmt &loop) {
        if (plan.boolean(loop.condition.get()) == false) {
            plan.unrolled.emplace(&loop, 0); // Preserve the initializer even for zero iterations.
            return;
        }
        const auto counted = match_counted_loop(loop);
        if (!counted || !unrollable_body(*loop.body, *counted->counter))
            return;
        if (const auto count = trip_count(*counted, loop.body->statements.size()))
            plan.unrolled.emplace(&loop, *count);
    }

    // ---- dead locals ----
    // Liveness is restricted to locals owned by one straight-line block. Any complex statement
    // is a barrier; calls and address-taking also disable removal for the block.

    // Classifies every surviving statement of the block, or returns nullopt if any of them is
    // outside what remove_dead_stores() models: declarations must all precede execution, every
    // declaration is a variable, and every initializer, assigned value and returned value only
    // reads variables. An initializer could otherwise expose an alias or call code that
    // observes an earlier local.
    std::optional<simple_block> classify(const block_stmt &block) const {
        simple_block result;
        bool seen_execution = false;
        for (const auto &statement : block.statements) {
            const stmt *node = statement.get();
            if (plan.omitted.contains(node))
                continue;
            if (const auto *item = dynamic_cast<const decl_stmt *>(node)) {
                const auto *variable = dynamic_cast<const variable_decl *>(item->declaration.get());
                if (seen_execution || !variable || !only_reads(variable->initializer.get()))
                    return std::nullopt;
                const auto *type =
                    dynamic_cast<const primitive_type_syntax *>(variable->declared_type.get());
                if (type && type->type == primitive_type::INT && !variable->immutable &&
                    variable->storage == variable_storage::ORDINARY)
                    result.candidates.insert(variable->name->lexeme);
                result.statements.push_back({simple_statement::kind::DECLARATION, node,
                                             &variable->name->lexeme, variable->initializer.get()});
                continue;
            }
            seen_execution = true;
            if (const auto assignment = as_simple_assignment(node)) {
                result.statements.push_back({simple_statement::kind::ASSIGNMENT, node,
                                             assignment->target, assignment->value});
            } else if (const auto *ret = dynamic_cast<const return_stmt *>(node)) {
                if (!only_reads(ret->value.get()))
                    return std::nullopt;
                result.statements.push_back(
                    {simple_statement::kind::RETURN, node, nullptr, ret->value.get()});
            } else {
                return std::nullopt;
            }
        }
        return result;
    }

    // Walks backwards: a read needs the preceding value, while an assignment kills that need.
    // Only known constant integer writes to candidates nobody reads later may be dropped.
    void remove_dead_stores(const simple_block &block) {
        std::unordered_set<std::string> live;
        // Targets of the assignments that survive. Declarations precede every assignment, so by
        // the time the walk reaches a declaration this holds all of them, and a binding that a
        // surviving assignment still writes to must stay.
        std::unordered_set<std::string> assigned;
        for (auto it = block.statements.rbegin(); it != block.statements.rend(); ++it) {
            const simple_statement &item = *it;
            if (item.what == simple_statement::kind::RETURN) {
                reads(item.value, live);
                continue;
            }
            const std::string &name = *item.name;
            const bool dead_constant =
                block.candidates.contains(name) && !live.contains(name) && integer(item.value);
            if (item.what == simple_statement::kind::ASSIGNMENT) {
                if (dead_constant) {
                    plan.omitted.insert(item.node);
                    continue;
                }
                assigned.insert(name);
            } else if (dead_constant && !assigned.contains(name)) {
                plan.omitted.insert(item.node);
            }
            live.erase(name);
            reads(item.value, live);
        }
    }

    void dead_locals(const block_stmt &block) {
        if (const auto simple = classify(block))
            remove_dead_stores(*simple);
    }

  public:
    explicit optimizer(optimization_options settings) : options(settings) {
    }
    optimization_plan run(const program &node) {
        if (options.enabled)
            node.accept(*this);
        return std::move(plan);
    }

    // TODO: no unused-function elimination: every declaration is kept and optimised even if
    // nothing calls it. Removing one needs a call graph from the entry point and care with
    // invariants and impl indexes, which hold pointers to declarations.
    void visit(const program &node) override {
        for (const auto &item : node.declarations)
            declaration(*item);
    }

    // ---- expressions ----

    void visit(const literal &node) override {
        record(node, node.value);
    }
    // TODO: no constant propagation: an identifier bound by `imut` to a constant is not
    // replaced by its value, so `imut n: int = 3; n + 1` is not folded. That needs the binding
    // each identifier resolves to, which only semantic analysis currently knows.
    void visit(const identifier &) override {
    }
    void visit(const this_expr &) override {
    }
    void visit(const binary_expr &node) override {
        expression(node.left.get());
        expression(node.right.get());
        if (auto value = fold(node))
            record(node, std::move(*value));
    }
    void visit(const unary_expr &node) override {
        expression(node.right.get());
        if (node.operation == unary_op::NEGATE) {
            if (const auto value = integer(node.right.get())) {
                if (const auto negated = as_int32(-static_cast<std::int64_t>(*value)))
                    record(node, *negated);
            }
        } else if (node.operation == unary_op::LOGICAL_NOT) {
            if (const auto value = plan.boolean(node.right.get()))
                record(node, !*value);
        }
    }
    void visit(const assignment_expr &node) override {
        expression(node.target.get());
        expression(node.value.get());
    }
    void visit(const postfix_expr &node) override {
        expression(node.obj.get());
        std::visit(overloaded{
                       [&](const expr_ptr &index) { expression(index.get()); },
                       [](const std::string &) {},
                       [&](const arg_list &args) {
                           for (const auto &arg : args)
                               expression(arg.get());
                       },
                       [&](const update_arguments &args) {
                           for (const auto &arg : args.regular)
                               expression(arg.get());
                           for (const auto &arg : args.state)
                               expression(arg.get());
                       },
                   },
                   node.arg);
    }
    void visit(const array_literal &node) override {
        for (const auto &element : node.elements)
            expression(element.get());
    }
    void visit(const struct_literal &node) override {
        for (const auto &field : node.init_list)
            expression(field.val.get());
    }
    void visit(const dynamic_alloc &node) override {
        expression(node.count.get());
    }
    void visit(const conversion_expr &node) override {
        expression(node.value.get());
    }

    // ---- statements ----

    void visit(const block_stmt &node) override {
        terminates = block(node);
    }
    void visit(const expr_stmt &node) override {
        expression(node.expression.get());
    }
    void visit(const decl_stmt &node) override {
        declaration(*node.declaration);
    }
    void visit(const return_stmt &node) override {
        expression(node.value.get());
        terminates = true;
    }
    // Branches after one whose condition is known true are unreachable, so they are neither
    // visited nor optimised; the code generator does not emit them.
    void visit(const if_stmt &node) override {
        bool all_return = true;
        for (const auto &branch : node.branches) {
            expression(branch.condition.get());
            const auto condition = plan.boolean(branch.condition.get());
            if (condition == false)
                continue;
            all_return = block(*branch.body) && all_return;
            if (condition == true) {
                terminates = all_return;
                return;
            }
        }
        terminates = node.else_body && block(*node.else_body) && all_return;
    }
    void visit(const while_stmt &node) override {
        expression(node.condition.get());
        if (plan.boolean(node.condition.get()) == false)
            plan.omitted.insert(&node);
        else
            block(*node.body);
    }
    void visit(const for_stmt &node) override {
        if (node.initializer)
            declaration(*node.initializer);
        expression(node.condition.get());
        expression(node.increment.get());
        block(*node.body);
        unroll(node);
    }
    // TODO: step bodies and reject handlers are skipped entirely, not even folded: their
    // declarations and writes feed invariant validation and the commit at the step boundary, so
    // optimising inside them needs to keep every state write and invariant input intact.
    void visit(const step_stmt &) override {
    }

    // ---- declarations ----

    void visit(const variable_decl &node) override {
        expression(node.initializer.get());
    }
    void visit(const function_decl &node) override {
        block(*node.body);
    }
    void visit(const struct_decl &) override {
    }
    void visit(const impl_decl &node) override {
        for (const auto &method : node.methods)
            declaration(*method);
    }
    void visit(const invariant_decl &node) override {
        expression(node.condition.get());
    }

    // ---- type syntax ----
    // Never reached: the optimiser does not descend into types, so array dimensions are not
    // folded here and are emitted as written.

    void visit(const primitive_type_syntax &) override {
    }
    void visit(const named_type_syntax &) override {
    }
    void visit(const pointer_type_syntax &) override {
    }
    void visit(const array_type_syntax &) override {
    }
};
} // namespace

std::optional<bool> optimization_plan::boolean(const expr *node) const {
    const auto found = constants.find(node);
    if (found != constants.end()) {
        if (const auto *value = std::get_if<bool>(&found->second))
            return *value;
    }
    return std::nullopt;
}

optimization_plan optimize(const program &node, optimization_options options) {
    return optimizer(options).run(node);
}
