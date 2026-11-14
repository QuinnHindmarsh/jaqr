#pragma once

#include <functional>

struct graph_node;

// One transition of the DFA definition: any char that matches leads to `to`.
struct edge {
    std::function<bool(char)> matches;
    const graph_node *to;
};
