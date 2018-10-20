#include "hlt/game.hpp"
#include "hlt/constants.hpp"
#include "hlt/log.hpp"

#include <random>
#include <ctime>
#include <set>
#include <unordered_set>
#include <algorithm>

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
    game.ready("ZanZanBot");

    enum Task { EXPLORE, RETURN };
    unordered_map<EntityId, Task> tasks;

    for (;;) {
        game.update_frame();
        shared_ptr<Player> me = game.me;
        unique_ptr<GameMap>& game_map = game.game_map;

        unordered_map<shared_ptr<Ship>, double> ships;
        vector<Command> command_queue;

        auto stuck = [&](shared_ptr<Ship> ship) {
            return ship->halite < game_map->at(ship)->halite / constants::MOVE_COST_RATIO ||
                (!ship->is_full() && game_map->at(ship)->halite >= constants::MAX_HALITE * 0.1);
        };

        auto cost = [&](shared_ptr<Ship> ship, Position p) {
            if (tasks[ship->id] == EXPLORE && p == me->shipyard->position)
                return numeric_limits<double>::max();

            MapCell* map_cell = game_map->at(p);
            Halite halite = map_cell->halite;
            int distance = game_map->calculate_distance(ship->position, p);

            return max(5, 5 * distance) * (constants::MAX_HALITE * 5.0 - halite);
        };

        auto evaluate = [&](shared_ptr<Ship> ship) {
            vector<Position> positions;
            if (tasks[ship->id] == RETURN) {
                ship->next = me->shipyard->position;
                for (auto& it : me->dropoffs)
                    positions.push_back(it.second->position);
            } else {
                for (vector<MapCell> &cells : game_map->cells)
                    for (MapCell& cell : cells)
                        positions.push_back(cell.position);
            }

            for (Position p : positions) {
                if (cost(ship, p) < cost(ship, ship->next))
                    ship->next = p;
            }
            if (tasks[ship->id] == RETURN)
                return -cost(ship, ship->next);
            return cost(ship, ship->next);
        };

        auto execute = [&](shared_ptr<Ship> ship) {
#if 0
            log::log(ship->id, "WANTS TO GO", ship->position.x, ship->position.y, "->", ship->next.x, ship->next.y);
#endif
            Direction d = game_map->naive_navigate(ship, ship->next);
            command_queue.push_back(ship->move(d));
        };

        // Tasks.
        for (const auto& ship_iterator : me->ships) {
            shared_ptr<Ship> ship = ship_iterator.second;
            EntityId id = ship->id;

            int closest_dropoff = game_map->calculate_distance(ship->position, me->shipyard->position);
            for (auto& it : me->dropoffs)
                closest_dropoff = min(closest_dropoff,
                        game_map->calculate_distance(ship->position, it.second->position));

            if (!tasks.count(id))
                tasks[id] = EXPLORE;
            if (tasks[id] == RETURN && ship->position == me->shipyard->position)
                tasks[id] = EXPLORE;
            if (tasks[id] == EXPLORE && ship->halite > constants::MAX_HALITE * 0.95)
                tasks[id] = RETURN;
            if (tasks[id] == EXPLORE &&
                    game.turn_number + closest_dropoff + (int) me->ships.size() / 4 >= constants::MAX_TURNS)
                tasks[id] = RETURN;

            if (stuck(ship)) {
                command_queue.push_back(ship->stay_still());
                game_map->at(ship)->mark_unsafe(ship);
            } else {
                ships[ship] = 0.0;
            }
        }

        while (!ships.empty()) {
            shared_ptr<Ship> ship;

            for (auto& it : ships) {
                if (!it.second) it.second = evaluate(it.first);
                if (!ship || it.second < ships[ship])
                    ship = it.first;
            }

            execute(ship);
            ships.erase(ship);
        }

        if (game.turn_number <= constants::MAX_TURNS * 0.50 && me->halite >= constants::SHIP_COST &&
                !game_map->at(me->shipyard)->is_occupied()) {
            command_queue.push_back(me->shipyard->spawn());
        }

#if 0
        for (auto& it : me->ships) {
            log::log(it.second->id, tasks[it.second->id] == EXPLORE ? "EXPLORE" : "RETURN");
        }
        for (Command c : command_queue)
            log::log(c);
#endif

        if (!game.end_turn(command_queue)) {
            break;
        }
    }

    return 0;
}
