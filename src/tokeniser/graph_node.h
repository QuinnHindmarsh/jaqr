#pragma once

#include "../tokens/token.h"
#include "edge.h"
#include <array>
#include <climits>
#include <cstddef>
#include <optional>
#include <vector>

// A DFA state. Edge targets borrow the static nodes in dfa.cpp, so cycles model repeated input
// without ownership cycles.
//
// The edges are the readable definition of the state; they are compiled once, at construction,
// into a table with one entry per char value, so the tokeniser does a single lookup per input
// char instead of calling every edge's matcher in turn. Only the targets' addresses are stored,
// so an edge may point at a node that is not constructed yet.
struct graph_node {
    // The first edge that matches a char decides where it goes, so specific edges must come
    // before general ones.
    graph_node(std::optional<token_type> accepted, const std::vector<edge> &edges)
        : type(accepted) {
        for (std::size_t i = 0; i < transitions.size(); ++i) {
            const char ch = static_cast<char>(i);
            for (const edge &candidate : edges) {
                if (candidate.matches(ch)) {
                    transitions[i] = candidate.to;
                    break;
                }
            }
        }
    }

    // the node reached by following ch, or nullptr if no edge matches ch
    const graph_node *next(char ch) const {
        return transitions[static_cast<unsigned char>(ch)];
    }

    std::optional<token_type> type; // empty = not an accepting state

  private:
    std::array<const graph_node *, std::size_t{1} << CHAR_BIT> transitions{};
};
