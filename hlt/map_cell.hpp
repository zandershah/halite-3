#pragma once

#include "dropoff.hpp"
#include "position.hpp"
#include "ship.hpp"
#include "types.hpp"

namespace hlt {

struct MapCell {
    Position position;
    Halite halite;
    std::shared_ptr<Ship> ship;
    // Only has dropoffs and shipyards. If id is -1,
    // then it's a shipyard, otherwise it's a dropoff.
    std::shared_ptr<Entity> structure;

    // Estimates for getting back to the nearest base.
    Halite cost_estimate = 0;
    int return_estimate = 0;
    Halite value_estimate = 0;

    MapCell(int x, int y, Halite halite) : position(x, y), halite(halite) {}

    bool is_empty() const { return !ship && !structure; }

    bool is_occupied() const { return static_cast<bool>(ship); }

    bool has_structure() const { return static_cast<bool>(structure); }

    void mark_unsafe(std::shared_ptr<Ship>& ship) { this->ship = ship; }
};

}  // namespace hlt
