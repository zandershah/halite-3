#include "hlt/game.hpp"

#include <bits/stdc++.h>

using namespace std;
using namespace hlt;

int main(int argc, char* argv[]) {
    hlt::Game game;
    // At this point "game" variable is populated with initial map data.
    // This is a good place to do computationally expensive start-up pre-processing.
    // As soon as you call "ready" function below, the 2 second per turn timer will start.
    game.ready("ZanZanBot");

    unordered_map<EntityId, Task> tasks;
    Halite q3;

    auto return_estimate = [&game](Position p) {
        pair<int, Position> estimate(game.game_map->calculate_distance(game.me->shipyard->position, p),
                game.me->shipyard->position);
        for (auto& it : game.me->dropoffs) {
            int e = game.game_map->calculate_distance(it.second->position, p);
            if (e < estimate.first) estimate = make_pair(e, it.second->position);
        }
        return estimate;
    };
    auto stuck = [&game, &q3](shared_ptr<Ship> ship) {
        return ship->halite < game.game_map->at(ship)->halite / constants::MOVE_COST_RATIO ||
            (!ship->is_full() && game.game_map->at(ship)->halite >= q3);
    };
    auto surrounding_halite = [&game](Position p, int falloff = 5) {
        Halite ret = game.game_map->at(p)->halite;
        for (Position pp : p.get_surrounding_cardinals())
            ret += game.game_map->at(pp)->halite / falloff;
        return ret;
    };
    auto evaluate = [&game, &tasks](std::shared_ptr<hlt::Ship> ship) {
        unique_ptr<GameMap>& game_map = game.game_map;

        // TODO: Using Dijkstras is dumb.
        vector<vector<Halite>> dist(game_map->height,
                vector<Halite>(game_map->width, numeric_limits<Halite>::max()));
        dist[ship->position.x][ship->position.y] = 0;
        {
            priority_queue<pair<Halite, Position>> pq;
            pq.emplace(0, ship->position);
            while (!pq.empty()) {
                Position p = pq.top().second;
                pq.pop();
                for (Position pp : p.get_surrounding_cardinals()) {
                    pp = game_map->normalize(pp);
                    Halite move_cost = game_map->at(p)->halite;
                    if (dist[p.x][p.y] + move_cost < dist[pp.x][pp.y]) {
                        dist[pp.x][pp.y] = dist[p.x][p.y] + move_cost;
                        pq.emplace(-dist[pp.x][pp.y], pp);
                    }
                }
            }
        }

        vector<Position> positions;

        switch (tasks[ship->id]) {
        case EXPLORE:
            for (vector<MapCell> &cells : game_map->cells) for (MapCell& cell : cells) {
                if (cell.value_estimate >= cell.cost_estimate)
                    positions.push_back(cell.position);
            }
            break;
        case RETURN:
        case HARD_RETURN:
            ship->next = game.me->shipyard->position;
            for (auto& it : game.me->dropoffs)
                positions.push_back(it.second->position);
            break;
        }

        auto cost = [&](shared_ptr<Ship> ship, Position p) {
            MapCell* map_cell = game_map->at(p);

            bool on_dropoff = map_cell->return_estimate == 0;
            if (tasks[ship->id] == EXPLORE && on_dropoff)
                return numeric_limits<double>::min();

            double turn_estimate = game_map->calculate_distance(ship->position, p) + map_cell->return_estimate;

            if (tasks[ship->id] & (RETURN | HARD_RETURN)) return -turn_estimate;

            Halite halite_cost_estimate = dist[p.x][p.y] + map_cell->cost_estimate;

            return (map_cell->value_estimate - halite_cost_estimate) / pow(max(5.0, turn_estimate), 0.5);
        };

        for (Position p : positions) {
            if (!game.game_map->at(p)->is_occupied() && cost(ship, ship->next) < cost(ship, p))
                ship->next = p;
        }
        if (tasks[ship->id] & (RETURN | HARD_RETURN))
            return -cost(ship, ship->next);
        return cost(ship, ship->next);
    };

    for (;;) {
        chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

        game.update_frame();
        shared_ptr<Player> me = game.me;
        unique_ptr<GameMap>& game_map = game.game_map;

        // Pre: Analysis of map state.
        {
            vector<Halite> flat_halite;
            flat_halite.reserve(game_map->height * game_map->width);
            for (vector<MapCell> &cells : game_map->cells) for (MapCell cell : cells)
                flat_halite.push_back(cell.halite);
            sort(flat_halite.begin(), flat_halite.end());
            q3 = flat_halite[flat_halite.size() * 3 / 4];
        }

        // Pre: Estimates for each cell.
        {
            vector<vector<Halite>> dist(game_map->height, vector<Halite>(game_map->width, numeric_limits<Halite>::max()));
            priority_queue<pair<Halite, Position>> pq;

            dist[me->shipyard->position.x][me->shipyard->position.y] = 0;
            pq.emplace(0, me->shipyard->position);
            for (auto& it : me->dropoffs) {
                dist[it.second->position.x][it.second->position.y] = 0;
                pq.emplace(0, it.second->position);
            }

            while (!pq.empty()) {
                Position p = pq.top().second;
                pq.pop();
                for (Position pp : p.get_surrounding_cardinals()) {
                    pp = game_map->normalize(pp);

                    const Halite move_cost = game_map->at(pp)->halite / constants::MOVE_COST_RATIO;
                    if (dist[p.x][p.y] + move_cost < dist[pp.x][pp.y]) {
                        dist[pp.x][pp.y] = dist[p.x][p.y] + move_cost;
                        pq.emplace(-dist[pp.x][pp.y], pp);
                    }
                }
            }

            for (auto& cells : game_map->cells) for (auto& cell : cells) {
                cell.value_estimate = surrounding_halite(cell.position, 5);
                cell.cost_estimate = dist[cell.position.x][cell.position.y];
                cell.return_estimate = return_estimate(cell.position).first;
            }
        }

        unordered_map<shared_ptr<Ship>, double> ships;
        vector<Command> command_queue;

        // Update tasks for each ship.
        {
            for (auto& it : me->ships) {
                shared_ptr<Ship> ship = it.second;
                EntityId id = ship->id;

                int closest_dropoff = game_map->at(ship)->return_estimate;

                // New ship.
                if (!tasks.count(id)) tasks[id] = EXPLORE;

                switch (tasks[id]) {
                case RETURN:
                    if (closest_dropoff == 0)
                        tasks[id] = EXPLORE;
                    break;
                case EXPLORE:
                    if (game.turn_number + closest_dropoff + me->ships.size() * 3 / 10 >= constants::MAX_TURNS)
                        tasks[id] = HARD_RETURN;
                    else if (ship->halite > constants::MAX_HALITE * 0.95)
                        tasks[id] = RETURN;
                    break;
                default:
                    break;
                }

                if (stuck(ship)) {
                    command_queue.push_back(ship->stay_still());
                    game_map->at(ship)->mark_unsafe(ship);
                } else {
                    ships[ship] = evaluate(ship);
                    // ships.insert(ship);
                }
            }
        }

        // Dispatch tasks.
        while (!ships.empty()) {
            shared_ptr<Ship> ship;
            // double fitness = numeric_limits<double>::max();

            for (auto& it : ships) {
                // double f = evaluate(game, it);
                if (!ship || ships[ship] < it.second)
                    ship = it.first;
            }

            Direction d = game_map->naive_navigate(ship, ship->next, tasks[ship->id]);
            command_queue.push_back(ship->move(d));
            ships.erase(ship);
        }

        if (game.turn_number <= constants::MAX_TURNS * 0.50 && me->halite >= constants::SHIP_COST &&
                !game_map->at(me->shipyard)->is_occupied()) {
            command_queue.push_back(me->shipyard->spawn());
        }

        chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
        log::log("Millis: ", chrono::duration_cast<chrono::milliseconds>(end - begin).count());

        if (!game.end_turn(command_queue)) {
            break;
        }
    }
}
