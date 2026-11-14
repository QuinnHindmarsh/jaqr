#include "tokeniser.h"
#include "dfa.h"
#include <cstddef>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

/*
outline of the tokeniser algorithm

maximal munch, with no backtracking

lines keep their '\n', so tokens (e.g. strings) can span lines

starting at line 0, col 0:
    start at start node at token_start
    follow transitions until the next char has no edge, or the end of the input
        an edge to the reject node raises an error at that char
    if stopped in a non-accepting state, raise an error at the char with no edge
        (or at the end of the input)
    add token_start..stop to the token list
    restart at the start node with token_start = stop

stopping in a non-accepting state is always an error, never a fallback to an earlier
accepting state. so 16b is an error, not INT_LITERAL '16' then IDENTIFIER 'b'.
*/

namespace {

struct position {
    std::size_t line;
    std::size_t col;
};

char char_at(const std::vector<std::vector<char>> &input, const position &pos) {
    return input[pos.line][pos.col];
}

// moves pos past the end of its line (and any empty lines) onto the next char
position skip_line_ends(const std::vector<std::vector<char>> &input, const position &start) {
    position pos = start;
    while (pos.line < input.size() && pos.col >= input[pos.line].size()) {
        pos = {pos.line + 1, 0};
    }
    return pos;
}

// the position of the char after pos, which may be on the next line
position advance(const std::vector<std::vector<char>> &input, const position &pos) {
    return skip_line_ends(input, {pos.line, pos.col + 1});
}

// builds the token covering [start, end), which may span multiple lines
token make_token(const std::vector<std::vector<char>> &input, token_type type,
                 const position &start, const position &end) {
    std::string lexeme;
    for (position pos = start; pos.line != end.line || pos.col != end.col;
         pos = advance(input, pos)) {
        lexeme += char_at(input, pos);
    }

    // TODO: see token.h; these casts go away once token stores std::size_t positions.
    return {type, lexeme, static_cast<int>(start.line), static_cast<int>(start.col)};
}

// shows '\n', '\t' and '\r' as escapes so they don't break the error message
std::string printable(char ch) {
    switch (ch) {
    case '\n':
        return "\\n";
    case '\t':
        return "\\t";
    case '\r':
        return "\\r";
    default:
        return {ch};
    }
}

// positions in error messages are 0-indexed
// TODO: editors and most compilers report 1-based lines and columns. Switching means changing
// both LEX messages here, the parser's SYN messages (which share the 0-based token positions)
// and the expectations in tests/tokeniser_tests.cpp and
// tests/tokeniser_parser_integration_tests.cpp together.
[[noreturn]] void report_unexpected_char(char ch, const position &pos) {
    throw std::runtime_error(std::format("LEX001: unexpected character '{}' at line {}, column {}",
                                         printable(ch), pos.line, pos.col));
}

[[noreturn]] void report_unexpected_end(const std::vector<std::vector<char>> &input,
                                        const position &token_start) {
    throw std::runtime_error(std::format(
        "LEX002: unexpected end of input at line {}, column {} (unfinished token starts at "
        "line {}, column {})",
        input.size() - 1, input.back().size(), token_start.line, token_start.col));
}

} // namespace

std::vector<token> tokenise(const std::vector<std::vector<char>> &input) {
    std::vector<token> tokens;
    position token_start = skip_line_ends(input, {0, 0});

    while (token_start.line < input.size()) {
        const graph_node *current_node = dfa_start;
        // exclusive end of the token
        position pos = token_start;

        while (pos.line < input.size()) {
            const char ch = char_at(input, pos);
            const graph_node *next = current_node->next(ch);

            if (next == dfa_reject) {
                report_unexpected_char(ch, pos);
            }
            // no edge for ch, so the token ends before it
            if (next == nullptr) {
                break;
            }

            current_node = next;
            pos = advance(input, pos);
        }

        const std::optional<token_type> &type = current_node->type;
        if (!type.has_value()) {
            if (pos.line < input.size()) {
                report_unexpected_char(char_at(input, pos), pos);
            }
            report_unexpected_end(input, token_start);
        }

        tokens.push_back(make_token(input, *type, token_start, pos));

        // restart directly after the token
        token_start = pos;
    }

    return tokens;
}
