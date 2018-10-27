#include "hlt/game.hpp"

#include <bits/stdc++.h>

using namespace std;
using namespace hlt;

unordered_map<EntityId, Task> tasks;
vector<vector<Halite>> halite;

double evaluate(Game& game, std::shared_ptr<hlt::Ship> ship) {
    unique_ptr<GameMap>& game_map = game.game_map;

    // TODO: Using Dijkstras is dumb.
    vector<vector<Halite>> dist(game_map->height,
            vector<Halite>(game_map->width, numeric_limits<Halite>::max()));
    dist[ship->position.x][ship->position.y] = 0;

    priority_queue<pair<Halite, Position>> pq;
    pq.emplace(0, ship->position);
    while (!pq.empty()) {
        Position p = pq.top().second;
        pq.pop();
        for (Position pp : p.get_surrounding_cardinals()) {
            pp = game_map->normalize(pp);
            if (dist[p.x][p.y] + halite[p.x][p.y] < dist[pp.x][pp.y]) {
                dist[pp.x][pp.y] = dist[p.x][p.y] + halite[p.x][p.y];
                pq.emplace(-dist[pp.x][pp.y], pp);
            }
        }
    }

    vector<Position> positions;
    if (tasks[ship->id] & (RETURN | HARD_RETURN)) {
        ship->next = game.me->shipyard->position;
        for (auto& it : game.me->dropoffs)
            positions.push_back(it.second->position);
    } else if (tasks[ship->id] == EXPLORE) {
        for (vector<MapCell> &cells : game_map->cells)
        for (MapCell& cell : cells)
            positions.push_back(cell.position);
    }

    auto cost = [&](shared_ptr<Ship> ship, Position p) {
        bool on_dropoff = p == game.me->shipyard->position;
        for (auto& it : game.me->dropoffs)
            on_dropoff |= p == it.second->position;

        if (tasks[ship->id] == EXPLORE && on_dropoff)
            return numeric_limits<double>::max();

        double turn_estimate = game_map->calculate_distance(ship->position, p);

        if (tasks[ship->id] & (RETURN | HARD_RETURN)) return turn_estimate;

        Halite halite_gain_estimate = game_map->at(p)->halite;
        for (Position pp : p.get_surrounding_cardinals()) {
            halite_gain_estimate += game_map->at(pp)->halite / 5;
        }
        Halite halite_cost_estimate = dist[p.x][p.y];

#if 0
        int back_distance = game_map->calculate_distance(p, game.me->shipyard->position);
        for (auto& it : game.me->dropoffs)
            back_distance = min(back_distance, game_map->calculate_distance(p, it.second->position));
        // turn_estimate += sqrt(back_distance);
#endif

        if (halite_cost_estimate >= halite_gain_estimate || 6000.0 <= halite_gain_estimate - halite_cost_estimate)
            return numeric_limits<double>::max();

        return (6000.0 - halite_gain_estimate + halite_cost_estimate) * sqrt(max(5.0, turn_estimate));
    };

    for (Position p : positions) {
        if (cost(ship, p) < cost(ship, ship->next))
            ship->next = p;
    }
    if (tasks[ship->id] & (RETURN | HARD_RETURN))
        return -cost(ship, ship->next);
    return cost(ship, ship->next);
}

int main(int argc, char* argv[]) {
    hlt::Game game;
    // At this point "game" variable is populated with initial map data.
    // This is a good place to do computationally expensive start-up pre-processing.
    // As soon as you call "ready" function below, the 2 second per turn timer will start.
    game.ready("ZanZanBot");
    halite = vector<vector<Halite>>(game.game_map->height, vector<Halite>(game.game_map->width));

    for (;;) {
        chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

        game.update_frame();
        shared_ptr<Player> me = game.me;
        unique_ptr<GameMap>& game_map = game.game_map;

        Halite q3;
        {
            vector<Halite> flat_halite;
            flat_halite.reserve(game_map->height * game_map->width);
            for (vector<MapCell> &cells : game_map->cells) {
                for (MapCell cell : cells) {
                    halite[cell.position.x][cell.position.y] = cell.halite;
                    flat_halite.push_back(cell.halite);
                }
            }
            sort(flat_halite.begin(), flat_halite.end());
            q3 = flat_halite[flat_halite.size() * 3 / 4];
        }

        unordered_map<shared_ptr<Ship>, double> ships;
        vector<Command> command_queue;
        auto stuck = [&](shared_ptr<Ship> ship) {
            return ship->halite < game_map->at(ship)->halite / constants::MOVE_COST_RATIO ||
                (!ship->is_full() && game_map->at(ship)->halite >= q3);
        };

        auto execute = [&](shared_ptr<Ship> ship) {
#if 0
            log::log(ship->id, "WANTS TO GO", ship->position.x, ship->position.y, "->", ship->next.x, ship->next.y);
#endif
            Direction d = game_map->naive_navigate(ship, ship->next, tasks[ship->id]);
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

            if (tasks[id] == RETURN && closest_dropoff == 0)
                tasks[id] = EXPLORE;
            if (tasks[id] == EXPLORE && ship->halite > constants::MAX_HALITE * 0.95)
                tasks[id] = RETURN;
            if (tasks[id] == EXPLORE &&
                    game.turn_number + closest_dropoff + (int) me->ships.size() * 0.3 >= constants::MAX_TURNS)
                tasks[id] = HARD_RETURN;

            // Dropoff.
            if (0) {
                Halite halite_around = 0;
                for (vector<MapCell> &cells : game_map->cells) {
                    for (MapCell cell : cells) {
                        if (game_map->calculate_distance(ship->position, cell.position) <= game_map->width * 0.1)
                            halite_around += cell.halite;
                    }
                }

                int local_ships = 0;
                for (auto& it : me->ships) {
                    if (game_map->calculate_distance(ship->position, it.second->position) <= 5)
                        ++local_ships;
                }
                bool local_dropoffs = game_map->calculate_distance(ship->position, me->shipyard->position) <= game_map->width * 0.25;
                for (auto& it : me->dropoffs) {
                    if (game_map->calculate_distance(ship->position, it.second->position) <= game_map->width * 0.5)
                        local_dropoffs = true;
                }

                if (game_map->at(ship)->halite >= constants::MAX_HALITE * 0.65 &&
                        halite_around >= constants::MAX_HALITE * 2.5 * game.players.size() &&
                        game_map->at(ship)->halite + ship->halite + me->halite >= constants::DROPOFF_COST &&
                        local_ships >= 3 && !local_dropoffs && game.turn_number <= constants::MAX_TURNS * 0.66 &&
                        me->dropoffs.size() < 2) {
                    me->halite -= max(0, game_map->at(ship)->halite + ship->halite);
                    command_queue.push_back(ship->make_dropoff());
                    me->dropoffs[-ship->id] = std::make_shared<Dropoff>(game.my_id, -ship->id, ship->position.x, ship->position.y);
                    log::log("DROPOFF!");
                    continue;
                }
            }


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
                if (!it.second) it.second = evaluate(game, it.first);
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
        chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
        log::log("Millis: ", chrono::duration_cast<chrono::milliseconds>(end - begin).count());

        if (!game.end_turn(command_queue)) {
            break;
        }
    }
}
