#pragma once

#include "game_map.hpp"
#include "player.hpp"
#include "types.hpp"

#include <bits/stdc++.h>

namespace hlt {

struct Game {
    int turn_number;
    PlayerId my_id;
    std::vector<std::shared_ptr<Player>> players;
    std::shared_ptr<Player> me;
    std::unique_ptr<GameMap> game_map;

    Game();
    void ready(const std::string& name);
    void update_frame();
    bool end_turn(const std::vector<Command>& commands);

    std::pair<int, hlt::Position> return_estimate(Position p) {
        std::pair<int, hlt::Position> estimate(
            game_map->calculate_distance(me->shipyard->position, p),
            me->shipyard->position);
        for (auto& it : me->dropoffs) {
            int e = game_map->calculate_distance(it.second->position, p);
            if (e < estimate.first)
                estimate = std::make_pair(e, it.second->position);
        }
        return estimate;
    }
};

}  // namespace hlt
