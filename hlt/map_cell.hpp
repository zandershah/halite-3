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

    Position closest_base;
    bool inspired = false;
    std::unordered_map<Direction, size_t> close_ships;

    MapCell(int x, int y, Halite halite)
        : position(x, y), halite(halite), closest_base(position) {}

    bool is_empty() const { return !ship && !structure; }

    bool is_occupied() const { return static_cast<bool>(ship); }

    bool has_structure() const { return static_cast<bool>(structure); }

    void mark_unsafe(std::shared_ptr<Ship>& ship) { this->ship = ship; }
};

}  // namespace hlt
