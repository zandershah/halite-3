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

    void compute_return_estimate(Position p) {
        Position& return_position_estimate =
            game_map->at(p)->return_position_estimate;
        return_position_estimate = me->shipyard->position;
        int& return_distance_estimate =
            game_map->at(p)->return_distance_estimate;
        return_distance_estimate =
            game_map->calculate_distance(p, return_position_estimate);

        for (auto& it : me->dropoffs) {
            int e = game_map->calculate_distance(it.second->position, p);
            if (e < return_distance_estimate) {
                return_distance_estimate = e;
                return_position_estimate = it.second->position;
            }
        }
    }
};

}  // namespace hlt
