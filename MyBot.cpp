#include "hlt/game.hpp"

#include <bits/stdc++.h>

using namespace std;
using namespace hlt;
using namespace constants;

const int HALITE_FALLOFF = 5;

struct ZanZanBot {
    Game game;
    unordered_map<EntityId, Task> tasks;
    Halite q3;

    unordered_map<EntityId, vector<vector<Halite>>> dijkstras;
    unordered_map<Position, bool> inspired_cache;

    unordered_map<int, unordered_map<int, double>> spawn_factor;

    bool stuck(shared_ptr<Ship> ship) {
        return ship->halite <
                   game.game_map->at(ship)->halite / MOVE_COST_RATIO ||
               (!ship->is_full() && game.game_map->at(ship)->halite >= q3);
    }

    bool inspired(Position p) {
        p = game.game_map->normalize(p);
        if (inspired_cache.find(p) != inspired_cache.end())
            return inspired_cache[p];
        int close_enemies = 0;
        for (auto& player : game.players) {
            if (player->id != game.my_id) {
                for (auto& it : player->ships) {
                    Position pp = it.second->position;
                    close_enemies += game.game_map->calculate_distance(p, pp) <=
                                     constants::INSPIRATION_RADIUS;
                }
            }
        }
        return inspired_cache[p] =
                   close_enemies >= constants::INSPIRATION_SHIP_COUNT;
    }

    Halite surrounding_halite(Position p) {
        Halite ret = game.game_map->at(p)->halite;
        for (Position pp : p.get_surrounding_cardinals())
            ret += game.game_map->at(pp)->halite / HALITE_FALLOFF;
        return ret;
    }

    double evaluate(std::shared_ptr<hlt::Ship> ship) {
        unique_ptr<GameMap>& game_map = game.game_map;

        if (dijkstras[ship->id].empty()) {
            vector<vector<Halite>> dist(
                game_map->height,
                vector<Halite>(game_map->width, numeric_limits<Halite>::max()));
            dist[ship->position.x][ship->position.y] = 0;
            {
                priority_queue<pair<Halite, Position>> pq;
                pq.emplace(0, ship->position);
                while (!pq.empty()) {
                    Position p = pq.top().second;
                    pq.pop();
                    const Halite move_cost =
                        game_map->at(p)->halite / MOVE_COST_RATIO;
                    for (Position pp : p.get_surrounding_cardinals()) {
                        pp = game_map->normalize(pp);
                        if (dist[p.x][p.y] + move_cost < dist[pp.x][pp.y]) {
                            dist[pp.x][pp.y] = dist[p.x][p.y] + move_cost;
                            pq.emplace(-dist[pp.x][pp.y], pp);
                        }
                    }
                }
            }
            dijkstras[ship->id] = move(dist);
        }
        vector<vector<Halite>>& dist = dijkstras[ship->id];

        vector<Position> positions;

        switch (tasks[ship->id]) {
            case EXPLORE:
                for (vector<MapCell>& cells : game_map->cells)
                    for (MapCell& cell : cells)
                        positions.push_back(cell.position);
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
            if (tasks[ship->id] == EXPLORE &&
                (on_dropoff || map_cell->halite == 0))
                return numeric_limits<double>::min();

            double turn_estimate =
                game_map->calculate_distance(ship->position, p) +
                map_cell->return_estimate;

            if (tasks[ship->id] & (RETURN | HARD_RETURN)) return -turn_estimate;

            Halite halite_profit_estimate = map_cell->value_estimate +
                                            map_cell->cost_estimate -
                                            dist[p.x][p.y];
            if (game_map->calculate_distance(ship->position, p) <=
                    INSPIRATION_RADIUS &&
                inspired(p))
                halite_profit_estimate +=
                    INSPIRED_BONUS_MULTIPLIER * map_cell->value_estimate;

            return halite_profit_estimate / max(1.0, turn_estimate);
        };

        for (Position p : positions) {
            if (cost(ship, ship->next) < cost(ship, p)) ship->next = p;
        }
        if (tasks[ship->id] & (RETURN | HARD_RETURN))
            return (game_map->width + cost(ship, ship->next)) * 1e3;
        return cost(ship, ship->next);
    }

    bool run();
};

bool ZanZanBot::run() {
    game.update_frame();
    shared_ptr<Player> me = game.me;
    unique_ptr<GameMap>& game_map = game.game_map;

    // Pre: Reset caches.
    {
        dijkstras.clear();
        inspired_cache.clear();
    }

    // Pre: Estimates for each cell.
    {
        vector<vector<Halite>> dist(
            game_map->height,
            vector<Halite>(game_map->width, numeric_limits<Halite>::max()));
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

                const Halite move_cost =
                    game_map->at(pp)->halite / MOVE_COST_RATIO;
                if (dist[p.x][p.y] + move_cost < dist[pp.x][pp.y]) {
                    dist[pp.x][pp.y] = dist[p.x][p.y] + move_cost;
                    pq.emplace(-dist[pp.x][pp.y], pp);
                }
            }
        }

        for (auto& cells : game_map->cells) {
            for (auto& cell : cells) {
                cell.value_estimate = surrounding_halite(cell.position);
                cell.cost_estimate = dist[cell.position.x][cell.position.y];
                cell.return_estimate =
                    game.return_estimate(cell.position).first;
            }
        }
    }

    // Pre: Analysis of map state.
    {
        // Halite distribution.
        vector<Halite> flat_halite;
        flat_halite.reserve(game_map->height * game_map->width);
        for (vector<MapCell>& cells : game_map->cells)
            for (MapCell cell : cells) flat_halite.push_back(cell.halite);
        sort(flat_halite.begin(), flat_halite.end());
        q3 = flat_halite[flat_halite.size() * 2 / 4];
    }

    unordered_map<shared_ptr<Ship>, double> ships;
    vector<Command> command_queue;

    // Update tasks for each ship.
    {
        double return_cutoff = 0.95;
        if (game.turn_number <= MAX_TURNS * 0.75) return_cutoff = 0.75;

        for (auto& it : me->ships) {
            shared_ptr<Ship> ship = it.second;
            EntityId id = ship->id;

            int closest_dropoff = game_map->at(ship)->return_estimate;

            // New ship.
            if (!tasks.count(id)) tasks[id] = EXPLORE;

            // TODO: Dry run of return.
            if (game.turn_number + closest_dropoff + me->ships.size() * 0.3 >=
                MAX_TURNS)
                tasks[id] = HARD_RETURN;

            switch (tasks[id]) {
                case EXPLORE:
                    if (ship->halite > MAX_HALITE * return_cutoff)
                        tasks[id] = RETURN;
                    break;
                case RETURN:
                    if (closest_dropoff == 0) tasks[id] = EXPLORE;
                    break;
                case HARD_RETURN:
                    break;
            }

                // Dropoff.
#ifdef USE_DROPOFF
            {
                Halite halite_around = 0;
                for (vector<MapCell>& cells : game_map->cells)
                    for (MapCell cell : cells) {
                        if (game_map->calculate_distance(ship->position,
                                                         cell.position) <=
                            game_map->width / 8)
                            halite_around += cell.halite;
                    }

                bool local_dropoffs =
                    game.return_estimate(ship->position).first <=
                    game_map->width / 3;

                bool ideal_dropoff =
                    halite_around >= MAX_HALITE * game_map->width / 4 &&
                    game_map->at(ship)->halite + ship->halite + me->halite >=
                        DROPOFF_COST &&
                    !local_dropoffs && game.turn_number <= MAX_TURNS * 0.666;

                if (ideal_dropoff ||
                    game_map->at(ship)->halite + ship->halite >= DROPOFF_COST) {
                    me->halite += game_map->at(ship)->halite + ship->halite -
                                  DROPOFF_COST;
                    command_queue.push_back(ship->make_dropoff());
                    game.me->dropoffs[-ship->id] = make_shared<Dropoff>(
                        game.my_id, -ship->id, ship->position.x,
                        ship->position.y);
                    log::log("DROPOFF!");
                    continue;
                }
            }
#endif

            if (stuck(ship) && tasks[ship->id] != HARD_RETURN) {
                command_queue.push_back(ship->stay_still());
                game_map->at(ship)->mark_unsafe(ship);
                game_map->mark_vis(ship->position, 1);
            } else {
                ships[ship] = evaluate(ship);
            }
        }
    }

    // TODO: Route all returns first.
    // Dispatch tasks.
    while (!ships.empty()) {
        shared_ptr<Ship> ship;

        for (auto& it : ships) {
            if (!ship || ships[ship] < it.second) ship = it.first;
        }

        // Update.
        {
            const Halite delta_halite = game_map->at(ship->next)->halite;
            game_map->at(ship->next)->value_estimate -= delta_halite;
            for (Position p : ship->next.get_surrounding_cardinals())
                game_map->at(p)->value_estimate -=
                    delta_halite / HALITE_FALLOFF;
        }

        // Execute.
        Direction d = Direction::STILL;
        switch (tasks[ship->id]) {
            case EXPLORE:
                d = game_map->naive_navigate(ship, tasks[ship->id]);
                break;
            case RETURN:
            case HARD_RETURN:
                d = game_map->navigate_return(ship, tasks[ship->id]);
                break;
        }
        command_queue.push_back(ship->move(d));
        ships.erase(ship);

        // Target would only move if it's value_estimate was modified.
        for (auto& it : ships) {
            if (game_map->calculate_distance(ship->next, it.first->next) <= 1)
                ships[it.first] = evaluate(it.first);
        }
    }

    size_t ship_count = 0;
    // TODO: Smarter counter of mid-game aggression.
    if (game.players.size() == 2 && game.turn_number <= MAX_TURNS * 0.95) {
        for (auto& player : game.players)
            if (player->id != game.my_id) ship_count = player->ships.size();
    }

    if (me->halite >= SHIP_COST &&
        !game_map->is_vis(me->shipyard->position, 1) &&
        (game.turn_number <=
             MAX_TURNS * spawn_factor[game_map->width][game.players.size()] ||
         me->ships.size() < ship_count)) {
        command_queue.push_back(me->shipyard->spawn());
    }

    return game.end_turn(command_queue);
}

int main(int argc, char* argv[]) {
    ZanZanBot z;

    // At this point "game" variable is populated with initial map data.
    // This is a good place to do computationally expensive start-up
    // pre-processing. As soon as you call "ready" function below, the 2 second
    // per turn timer will start.
    z.game.ready("ZanZanBot");

    z.spawn_factor[32][2] = 0.5;
    z.spawn_factor[40][2] = 0.5;
    z.spawn_factor[48][2] = 0.5;
    z.spawn_factor[56][2] = 0.55;
    z.spawn_factor[64][2] = 0.675;

    z.spawn_factor[32][4] = 0.325;
    z.spawn_factor[40][4] = 0.375;
    z.spawn_factor[48][4] = 0.5;
    z.spawn_factor[56][4] = 0.5;
    z.spawn_factor[64][4] = 0.525;

    for (;;) {
        auto begin = std::chrono::steady_clock::now();
        if (!z.run()) break;
        auto end = std::chrono::steady_clock::now();
        log::log(
            "Millis: ",
            chrono::duration_cast<chrono::milliseconds>(end - begin).count());
    }
}
