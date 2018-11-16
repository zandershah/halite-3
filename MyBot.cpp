#include "MyBot.h"
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

    unordered_map<EntityId, int> last_moved;

    bool stuck(Halite ship_halite, Halite left_halite, bool is_full) {
        return ship_halite < left_halite / MOVE_COST_RATIO ||
               (!is_full && left_halite >= q3);
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

    Direction navigate_return(shared_ptr<Ship> ship);

    double evaluate(shared_ptr<Ship> ship);

    bool run();
};

Direction ZanZanBot::navigate_return(shared_ptr<Ship> ship) {
    unique_ptr<GameMap>& game_map = game.game_map;
    // Position represents last.
    vector<vector<State>> dp(game_map->height, vector<State>(game_map->width));
    // Position represents current.
    priority_queue<State, vector<State>, StateCompare> pq;
    pq.emplace(0, ship->halite, game_map->normalize(ship->position));
    dp[pq.top().p.x][pq.top().p.y] = pq.top();
    while (!pq.empty()) {
        State s = pq.top();
        pq.pop();

        if (s.p == ship->next) break;
        if (dp[s.p.x][s.p.y] < s) continue;

        // Try to wait 'n' turns.
        Halite ship_halite = s.h;
        Halite left_halite = game_map->at(s.p)->halite;
        for (size_t i = 0; i <= 5; ++i) {
            const Halite move_cost = left_halite / MOVE_COST_RATIO;

            if (game_map->is_vis(s.p, s.t + i)) break;
            if (ship_halite < move_cost) continue;

            State ss(s.t + i + 1, ship_halite - move_cost, s.p);
            for (Position p : s.p.get_surrounding_cardinals()) {
                p = game_map->normalize(p);
                if (ss < dp[p.x][p.y] && !game_map->is_vis(p, ss.t)) {
                    dp[p.x][p.y] = ss;
                    pq.emplace(ss.t, ss.h, p);
                }
            }

            Halite delta_halite =
                (left_halite + EXTRACT_RATIO - 1) / EXTRACT_RATIO;
            delta_halite = min(delta_halite, MAX_HALITE - ship_halite);

            ship_halite += delta_halite;
            left_halite -= delta_halite;
        }
    }

    vector<Position> path;
    path.push_back(game_map->normalize(ship->next));
    while (path.back() != ship->position) {
        State& s = dp[path.back().x][path.back().y];
        if (s.t == numeric_limits<int>::max()) break;
        for (size_t t = s.t; t > dp[s.p.x][s.p.y].t; --t) path.push_back(s.p);
    }

    if (path.back() != ship->position || path.size() == 1) {
        if (tasks[ship->id] != HARD_RETURN)
            game_map->mark_vis(ship->position, 1);
        return Direction::STILL;
    }

    reverse(path.begin(), path.end());
    for (size_t i = 0; i < path.size(); ++i) {
        if (tasks[ship->id] != HARD_RETURN || i + 1 != path.size())
            game_map->mark_vis(path[i], i);
    }

    for (Direction d : ALL_CARDINALS) {
        if (game_map->normalize(path[0].directional_offset(d)) == path[1])
            return d;
    }
    return Direction::STILL;
}

double ZanZanBot::evaluate(shared_ptr<Ship> ship) {
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
    for (vector<MapCell>& cells : game_map->cells)
        for (MapCell& cell : cells) positions.push_back(cell.position);

    auto cost = [&](shared_ptr<Ship> ship, Position p) {
        MapCell* map_cell = game_map->at(p);

        bool on_dropoff = map_cell->return_distance_estimate == 0;
        if (on_dropoff || map_cell->halite == 0)
            return numeric_limits<double>::min();

        unsigned int d = game_map->calculate_distance(ship->position, p);
        double turn_estimate = d + map_cell->return_distance_estimate;

        Halite halite_profit_estimate =
            map_cell->value_estimate + map_cell->cost_estimate - dist[p.x][p.y];
        if (d <= INSPIRATION_RADIUS && inspired(p))
            halite_profit_estimate +=
                INSPIRED_BONUS_MULTIPLIER * map_cell->halite;

        return halite_profit_estimate / max(1.0, turn_estimate);
    };

    for (Position p : positions) {
        if (cost(ship, ship->next) < cost(ship, p)) ship->next = p;
    }
    return cost(ship, ship->next);
}

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
                game.compute_return_estimate(cell.position);
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

    unordered_map<shared_ptr<Ship>, double> explorers;
    vector<shared_ptr<Ship>> returners;
    vector<Command> command_queue;

    // Update tasks for each ship.
    {
        double return_cutoff = 0.95;
        if (game.turn_number <= MAX_TURNS * 0.75 && game.players.size() == 2)
            return_cutoff = 0.75;

        for (auto& it : me->ships) {
            shared_ptr<Ship> ship = it.second;
            EntityId id = ship->id;

            int closest_dropoff = game_map->at(ship)->return_distance_estimate;

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
            {
                Halite halite_around = 0;
                for (vector<MapCell>& cells : game_map->cells)
                    for (MapCell cell : cells) {
                        if (game_map->calculate_distance(ship->position,
                                                         cell.position) <=
                            game_map->width / 8)
                            halite_around += cell.halite;
                    }

                int d = game_map->width / (game.players.size() == 2 ? 3 : 6);
                bool local_dropoffs =
                    game_map->at(ship)->return_distance_estimate <= d;

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

                    // Fix return_estimate.
                    for (auto& cells : game_map->cells)
                        for (auto& cell : cells)
                            game.compute_return_estimate(cell.position);

                    log::log("DROPOFF!");
                    continue;
                }
            }

            // TODO: Fix stuck.
            if (stuck(ship->halite, game_map->at(ship)->halite,
                      ship->is_full()) &&
                tasks[ship->id] != HARD_RETURN) {
                command_queue.push_back(ship->stay_still());

                Halite ship_halite = ship->halite;
                Halite left_halite = game_map->at(ship)->halite;
                // Figure out how many turns the ship will be stuck.
                for (int i = 1;
                     stuck(ship_halite, left_halite, ship_halite == MAX_HALITE);
                     ++i) {
                    game_map->mark_vis(ship->position, i);

                    Halite delta_halite =
                        (left_halite + EXTRACT_RATIO - 1) / EXTRACT_RATIO;
                    delta_halite = min(delta_halite, MAX_HALITE - ship_halite);

                    ship_halite += delta_halite;
                    left_halite -= delta_halite;
                }
            } else if (tasks[ship->id] == EXPLORE) {
                explorers[ship] = evaluate(ship);
            } else {
                returners.push_back(ship);
            }
        }
    }

    // Route returners.
    sort(returners.begin(), returners.end(),
         [&](shared_ptr<Ship> u, shared_ptr<Ship> v) {
             return game_map->calculate_distance(u->position, u->next) <
                    game_map->calculate_distance(v->position, v->next);
         });
    for (shared_ptr<Ship> ship : returners) {
        ship->next = game_map->at(ship)->return_position_estimate;
        Direction d = navigate_return(ship);
        command_queue.push_back(ship->move(d));
    }

    // Route explorers.
    while (!explorers.empty()) {
        shared_ptr<Ship> ship;

        for (auto& it : explorers) {
            if (!ship || explorers[ship] < it.second) ship = it.first;
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
        Direction d = game_map->naive_navigate(ship, tasks[ship->id]);
        command_queue.push_back(ship->move(d));
        explorers.erase(ship);

        if (d != Direction::STILL)
            last_moved[ship->id] = game.turn_number;
        else if (game.turn_number - last_moved[ship->id] >= 10)
            tasks[ship->id] = RETURN;

        // Target would only move if it's value_estimate was modified.
        for (auto& it : explorers) {
            if (game_map->calculate_distance(ship->next, it.first->next) <= 1)
                explorers[it.first] = evaluate(it.first);
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
    z.spawn_factor[64][2] = 0.625;

    z.spawn_factor[32][4] = 0.35;
    z.spawn_factor[40][4] = 0.375;
    z.spawn_factor[48][4] = 0.5;
    z.spawn_factor[56][4] = 0.525;
    z.spawn_factor[64][4] = 0.525;

    for (;;) {
        auto begin = chrono::steady_clock::now();
        if (!z.run()) break;
        auto end = chrono::steady_clock::now();
        log::log(
            "Millis: ",
            chrono::duration_cast<chrono::milliseconds>(end - begin).count());
    }
}
