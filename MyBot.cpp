#include "hlt/game.hpp"
#include "hlt/constants.hpp"
#include "hlt/log.hpp"

#include <bits/stdc++.h>

using namespace std;
using namespace hlt;

int main(int argc, char* argv[]) {
    unsigned int rng_seed;
    if (argc > 1) {
        rng_seed = static_cast<unsigned int>(stoul(argv[1]));
    } else {
        rng_seed = static_cast<unsigned int>(time(nullptr));
    }
    mt19937 rng(rng_seed);

    Game game;
    // At this point "game" variable is populated with initial map data.
    // This is a good place to do computationally expensive start-up pre-processing.
    // As soon as you call "ready" function below, the 2 second per turn timer will start.
    game.ready("ZanderBotCpp");

    while (1) {
        game.update_frame();
        shared_ptr<Player> me = game.me;
        unique_ptr<GameMap> &game_map = game.game_map;

        // Assign.
        for (const auto &it : me->ships) {
            shared_ptr<Ship> ship = it.second;
            MapCell *map_cell = game_map->at(ship->position);

            if (ship->task == kReturning)
              continue;

            // Check if the ship has enough energy.
            if ((ship->halite >= constants::MAX_HALITE * 2 / 3 ||
                game.ending()) &&
                ship->halite > map_cell->halite / constants::MOVE_COST_RATIO)
                ship->task = kReturning;
            else
                ship->task = kExploring;
        }

        // Evaluate.
        vector<Command> command_queue;

        for (const auto& ship_iterator : me->ships) {
            shared_ptr<Ship> ship = ship_iterator.second;
            Position p = ship->position;
            MapCell *map_cell = game_map->at(p);

            // Execute Job.
            Direction d;
            if (ship->task == kReturning) {
                // Return to shipyard. Self-destruct in endgame.
                d = game_map->naive_navigate(ship, me->shipyard->position);
                if (game.ending() && game_map->calculate_distance(p, me->shipyard->position) <= 1) {
                    for (int i = 0; i < 4; ++i) {
                        if (p.directional_offset(ALL_CARDINALS[i]) == me->shipyard->position)
                            d = ALL_CARDINALS[i];
                    }
                }
            } else {
              for (int i = -game_map->height / game.players.size(); i <= game_map->height / game.players.size(); ++i) {
                for (int j = -game_map->width / game.players.size(); i <= game_map->width/ game.players.size(); ++i) {
                }
              }

                for (int i = 0; i < 1000; ++i) {
                    d = ALL_CARDINALS[rng() % 4];
                    MapCell *new_map_cell = game_map->at(ship->position.directional_offset(d));
                    if (!new_map_cell->is_occupied())
                        break;
                    d = Direction::STILL;
                }
            }

            if ((map_cell->halite / 10 > ship->halite || map_cell->halite >= constants::MAX_HALITE / 10) && !ship->is_full()) {
                d = Direction::STILL;
            }

            command_queue.push_back(ship->move(d));
        }

        if (game.turn_number <= constants::MAX_TURNS / 2 &&
            me->halite >= constants::SHIP_COST &&
            me->ships.size() <= 1u &&
            !game_map->at(me->shipyard)->is_occupied()) {
            // Hack fix, proper solution is to have real navigation.
            int full = 0;
            for (Position p : me->shipyard->position.get_surrounding_cardinals()) {
                full += game_map->at(p)->is_occupied();
            }
            if (full <= 2) {
                command_queue.push_back(me->shipyard->spawn());
            }
        }

        if (!game.end_turn(command_queue)) {
            break;
        }
    }

    return 0;
}
