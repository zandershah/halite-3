#pragma once

#include "hlt/position.hpp"
#include "hlt/types.hpp"

#include <bits/stdc++.h>

struct State {
    State() : p(0, 0) {}
    State(size_t t, hlt::Halite h, hlt::Position p) : t(t), h(h), p(p) {}
    size_t t = std::numeric_limits<int>::max();
    hlt::Halite h = 0;
    hlt::Position p;

    bool operator<(State& s) const {
        if (t == s.t) return h > s.h;
        return t < s.t;
    }
};

struct StateCompare {
    // Reversed for priority_queue.
    bool operator()(State& u, State& v) const { return v < u; }
};
