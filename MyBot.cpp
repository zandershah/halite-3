#include "hlt/game.hpp"
#include "hlt/constants.hpp"
#include "hlt/log.hpp"

#include <bits/stdc++.h>

using namespace std;
using namespace hlt;

enum State {
    kExploring,
    kReturning
};

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

    log::log("Successfully created bot! My Player ID is " + to_string(game.my_id) + ". Bot rng seed is " + to_string(rng_seed) + ".");

    // TODO: Tutorial Code.
    unordered_map<int, State> ship_state;

    while (1) {
        game.update_frame();
        shared_ptr<Player> me = game.me;
        unique_ptr<GameMap> &game_map = game.game_map;

        // Mark all ships.
        for (const shared_ptr<Player> &p : game.players) {
            for (const auto &ship_it : p->ships) {
                shared_ptr<Ship> ship = ship_it.second;
                MapCell *map_cell = game_map->at(ship->position);
                map_cell->mark_unsafe(ship);
            }
        }

        vector<Command> command_queue;

        for (const auto& ship_iterator : me->ships) {
            shared_ptr<Ship> ship = ship_iterator.second;
            State& s = ship_state[ship->id];
            MapCell *map_cell = game_map->at(ship->position);

            // Update Job.
            if (!s) {
                s = kExploring;
            }
            if (s == kExploring && ship->halite >= constants::MAX_HALITE / 4) {
                s = kReturning;
            } else if (s == kReturning && ship->position == me->shipyard->position) {
                s = kExploring;
            }
            if (game.turn_number > 350) {
                s = kReturning;
            }

            // Execute Job.
            Direction d;
            if (s == kReturning) {
                d = game_map->naive_navigate(ship, me->shipyard->position);

                if (game.turn_number > 350 && game_map->calculate_distance(ship->position, me->shipyard->position) <= 1) {
                    vector<Direction> ds = game_map->get_unsafe_moves(ship->position, me->shipyard->position);
                    if (!ds.empty()) {
                        d = ds.front();
                    }
                }
            } else {
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
            map_cell->mark_safe();

            map_cell = game_map->at(ship->position.directional_offset(d));
            map_cell->mark_unsafe(ship);

            command_queue.push_back(ship->move(d));
        }

        if (game.turn_number <= 300 &&
            me->halite >= constants::SHIP_COST * max(1, game.turn_number / 100) &&
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
