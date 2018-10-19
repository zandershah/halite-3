#pragma once

#include "entity.hpp"
#include "constants.hpp"
#include "command.hpp"

#include <memory>
#include <vector>

namespace hlt {
    struct Ship : Entity {
        Halite halite;
        Task task;
        std::vector<Direction> path;

        Ship(PlayerId player_id, EntityId ship_id, int x, int y, Halite halite) :
            Entity(player_id, ship_id, x, y),
            halite(halite), task(Task::kNone)
        {}

        bool is_full() const {
            return halite >= constants::MAX_HALITE;
        }

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
}

namespace std {
    template <>
    struct hash<hlt::Ship> {
        std::size_t operator()(const hlt::Ship& ship) const {
            return ship.id;
        }
    };
}
