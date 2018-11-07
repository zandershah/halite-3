#pragma once

#include "command.hpp"
#include "constants.hpp"
#include "entity.hpp"

#include <bits/stdc++.h>

namespace hlt {

struct Ship : Entity {
    Halite halite;
    Position next;

    Ship(PlayerId player_id, EntityId ship_id, int x, int y, Halite halite)
        : Entity(player_id, ship_id, x, y), halite(halite), next(x, y) {}

    bool is_full() const { return halite >= constants::MAX_HALITE; }

    Command make_dropoff() const {
        return hlt::command::transform_ship_into_dropoff_site(id);
    }

    Command move(Direction direction) const {
        return hlt::command::move(id, direction);
    }

    Command stay_still() const {
        return hlt::command::move(id, Direction::STILL);
    }

    static std::shared_ptr<Ship> _generate(PlayerId player_id);
};

}  // namespace hlt

namespace std {

template <>
struct hash<hlt::Ship> {
    std::size_t operator()(const hlt::Ship& ship) const { return ship.id; }
};

}  // namespace std
