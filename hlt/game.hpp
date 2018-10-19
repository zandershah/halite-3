#pragma once

#include "game_map.hpp"
#include "player.hpp"
#include "types.hpp"

#include <vector>
#include <iostream>

namespace hlt {
    struct Game {
        int turn_number;
        PlayerId my_id;
        std::vector<std::shared_ptr<Player>> players;
        std::shared_ptr<Player> me;
        std::unique_ptr<GameMap> game_map;
        char collision[505][65][65];

        Game();
        void ready(const std::string& name);
        void update_frame();
        bool end_turn(const std::vector<Command>& commands);

        bool should_return(std::shared_ptr<Ship> ship) const;

        int compute_move(std::shared_ptr<Ship> ship);
        int compute_explore(std::shared_ptr<Ship> ship);
        int compute_return(std::shared_ptr<Ship> ship);

        Command make_move(std::shared_ptr<Ship> ship);
        bool should_spawn();
    };
}
