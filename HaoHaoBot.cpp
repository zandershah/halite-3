#include "HaoHaoBot.h"
#include "hlt/game.hpp"

#include <bits/stdc++.h>

using namespace std;
using namespace hlt;
using namespace constants;
using namespace chrono;

Game game;
unordered_map<EntityId, Task> tasks;
unordered_map<int, unordered_map<int, double>> spawn_factor;
unordered_map<EntityId, int> last_moved;

Direction navigate_return(shared_ptr<Ship> ship) {
    unique_ptr<GameMap>& game_map = game.game_map;

    set<Position> goals;
    goals.insert(game.me->shipyard->position);
    ship->next = game.me->shipyard->position;
    for (auto& it : game.me->dropoffs) goals.insert(it.second->position);
    // TODO Fix goals;
    for (Position p : goals) {
        if (game_map->calculate_distance(ship->next, ship->position) <
            game_map->calculate_distance(ship->next, p))
            ship->next = p;
    }

    // Position represents last.
    vector<vector<State>> dp(game_map->height, vector<State>(game_map->width));
    // Position represents current.
    priority_queue<State, vector<State>, StateCompare> pq;
    pq.emplace(0, ship->halite, game_map->normalize(ship->position));
    dp[pq.top().p.x][pq.top().p.y] = pq.top();
    while (!pq.empty()) {
        State s = pq.top();
        pq.pop();

        if (goals.count(s.p)) goals.erase(s.p);
        if (goals.empty()) break;
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

int main(int argc, char* argv[]) {
    unsigned int rng_seed;
    if (argc > 1) {
        rng_seed = static_cast<unsigned int>(stoul(argv[1]));
    } else {
        rng_seed = static_cast<unsigned int>(time(nullptr));
    }
    mt19937 rng(rng_seed);

    // At this point "game" variable is populated with initial map data.
    // This is a good place to do computationally expensive start-up
    // pre-processing. As soon as you call "ready" function below, the 2
    // second per turn timer will start.
    game.ready("HaoHaoBot");

    spawn_factor[32][2] = 0.5;
    spawn_factor[40][2] = 0.5;
    spawn_factor[48][2] = 0.5;
    spawn_factor[56][2] = 0.55;
    spawn_factor[64][2] = 0.625;

    spawn_factor[32][4] = 0.35;
    spawn_factor[40][4] = 0.375;
    spawn_factor[48][4] = 0.5;
    spawn_factor[56][4] = 0.525;
    spawn_factor[64][4] = 0.525;

    while (1) {
        auto begin = steady_clock::now();

        game.update_frame();
        shared_ptr<Player> me = game.me;
        unique_ptr<GameMap>& game_map = game.game_map;

        unordered_map<Position, bool> inspired;
        unordered_map<Position, int> closest_spawn;
        set<Position> positions;
        Halite halite_cutoff;

        auto evaluate = [&](Position p, shared_ptr<Ship> ship) {
            int spawn_distance = closest_spawn[p];

            if (!spawn_distance) return numeric_limits<double>::min();

            Halite halite_profit_estimate = game_map->at(p)->halite;
            if (spawn_distance <= INSPIRATION_RADIUS && inspired[p])
                halite_profit_estimate +=
                    INSPIRED_BONUS_MULTIPLIER * game_map->at(p)->halite;

            return halite_profit_estimate * 1.0 /
                   (spawn_distance +
                    game_map->calculate_distance(ship->position, p));
        };

        auto stuck = [&](Halite ship_halite, Halite left_halite, bool is_full) {
            if (!left_halite || is_full) return false;
            return ship_halite < left_halite / MOVE_COST_RATIO ||
                   left_halite >= halite_cutoff;
        };

        // Pre.
        {
            vector<Halite> flat_halite;
            flat_halite.reserve(game_map->height * game_map->width);

            for (auto& cells : game_map->cells) {
                for (auto& cell : cells) {
                    Position p = cell.position;

                    flat_halite.push_back(cell.halite);

                    int close_enemies = 0;
                    for (auto& player : game.players) {
                        if (player->id == game.my_id) continue;
                        for (auto& it : player->ships) {
                            close_enemies += game_map->calculate_distance(
                                                 p, it.second->position) <=
                                             INSPIRATION_RADIUS;
                        }
                    }
                    inspired[p] = close_enemies >= INSPIRATION_SHIP_COUNT;

                    int spawn_distance =
                        game_map->calculate_distance(p, me->shipyard->position);
                    for (auto& it : me->dropoffs) {
                        spawn_distance =
                            min(spawn_distance, game_map->calculate_distance(
                                                    p, it.second->position));
                    }
                    closest_spawn[p] = spawn_distance;

                    positions.insert(p);
                }
            }

            sort(flat_halite.begin(), flat_halite.end());
            halite_cutoff = flat_halite[flat_halite.size() * 2 / 4];
        }

        for (shared_ptr<Player> player : game.players) {
            for (auto& it : player->ships) {
                auto ship = it.second;
                if (ship->owner == game.my_id) continue;

                Position& p = ship->position;
                if (game.players.size() == 4 && closest_spawn[p]) {
                    positions.erase(p);

                    game_map->mark_vis(p, 1);
                    for (Position pp : p.get_surrounding_cardinals())
                        game_map->mark_vis(pp, 1);
                }
            }
        }

        set<shared_ptr<Ship>> explorers;
        vector<shared_ptr<Ship>> returners;
        vector<Command> command_queue;

        // Task assignment.
        {
            double return_cutoff = 0.95;
            if (game.turn_number <= MAX_TURNS * 0.75 &&
                game.players.size() == 2)
                return_cutoff = 0.75;

            for (auto& it : me->ships) {
                shared_ptr<Ship> ship = it.second;
                EntityId id = ship->id;

                int spawn_distance = closest_spawn[ship->position];

                // New ship.
                if (!tasks.count(id)) tasks[id] = EXPLORE;

                // TODO: Dry run of return.
                if (game.turn_number + spawn_distance +
                        me->ships.size() * 0.3 >=
                    MAX_TURNS)
                    tasks[id] = HARD_RETURN;

                switch (tasks[id]) {
                    case EXPLORE:
                        if (ship->halite > MAX_HALITE * return_cutoff)
                            tasks[id] = RETURN;
                        break;
                    case RETURN:
                        if (!spawn_distance) tasks[id] = EXPLORE;
                        break;
                    case HARD_RETURN:
                        break;
                }

#if 0
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

                int d = game_map->width / (game.players.size() == 2 ? 3 : 4);
                bool local_dropoffs =
                    game_map->at(ship)->return_distance_estimate <= d;

                bool ideal_dropoff =
                    halite_around >= MAX_HALITE * game_map->width / 3 &&
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
#endif

                // TODO: Fix stuck.
                if (stuck(ship->halite, game_map->at(ship)->halite,
                          ship->is_full()) &&
                    tasks[ship->id] != HARD_RETURN) {
                    command_queue.push_back(ship->stay_still());

                    positions.erase(ship->position);

                    Halite ship_halite = ship->halite;
                    Halite left_halite = game_map->at(ship)->halite;
                    // Figure out how many turns the ship will be stuck.
                    for (int i = 1; stuck(ship_halite, left_halite,
                                          ship_halite == MAX_HALITE);
                         ++i) {
                        game_map->mark_vis(ship->position, i);

                        Halite delta_halite =
                            (left_halite + EXTRACT_RATIO - 1) / EXTRACT_RATIO;
                        delta_halite =
                            min(delta_halite, MAX_HALITE - ship_halite);

                        ship_halite += delta_halite;
                        left_halite -= delta_halite;
                    }
                } else if (tasks[ship->id] == EXPLORE) {
                    explorers.insert(ship);
                } else {
                    returners.push_back(ship);
                }
            }
        }

        // Route returners.
        sort(returners.begin(), returners.end(),
             [&](shared_ptr<Ship> u, shared_ptr<Ship> v) {
                 if (closest_spawn[u->position] == closest_spawn[v->position])
                     return u->id < v->id;
                 return closest_spawn[u->position] < closest_spawn[v->position];
             });
        for (shared_ptr<Ship> ship : returners) {
            Direction d = navigate_return(ship);
            command_queue.push_back(ship->move(d));
        }

        vector<shared_ptr<Ship>> explorers_queue;
        // Route explorers.
        while (!explorers.empty()) {
            shared_ptr<Ship> best_ship;
            Position best_p(0, 0);
            double val = 0.0;

            for (Position p : positions) {
                for (shared_ptr<Ship> ship : explorers) {
                    if (val < evaluate(p, ship)) {
                        val = evaluate(p, ship);
                        best_ship = ship;
                        best_p = p;
                    }
                }
            }

            best_ship->next = best_p;
            positions.erase(best_ship->next);
            explorers.erase(best_ship);
            explorers_queue.push_back(best_ship);
        }

        for (;;) {
            auto t = duration_cast<milliseconds>(steady_clock::now() - begin);
            if (t.count() > 500) break;

            bool v = false;
            for (int i = 0; i < explorers_queue.size(); ++i) {
                for (int j = i + 1; j < explorers_queue.size(); ++j) {
                    shared_ptr<Ship> ship_i = explorers_queue[i];
                    shared_ptr<Ship> ship_j = explorers_queue[j];

                    if (evaluate(ship_i->next, ship_j) +
                            evaluate(ship_j->next, ship_i) >
                        evaluate(ship_i->next, ship_i) +
                            evaluate(ship_j->next, ship_j)) {
                        v = true;
                        swap(ship_i->next, ship_j->next);
                    }
                }
            }
            if (!v) break;
        }
        for (shared_ptr<Ship> ship : explorers_queue) {
            Direction d = game_map->naive_navigate(ship, tasks[ship->id]);
            command_queue.push_back(ship->move(d));

            if (d != Direction::STILL)
                last_moved[ship->id] = game.turn_number;
            else if (game.turn_number - last_moved[ship->id] >= 10)
                tasks[ship->id] = RETURN;
        }

        size_t ship_count = 0;
        if (game.turn_number <= MAX_TURNS * 0.95) {
            ship_count = numeric_limits<size_t>::max();
            for (auto& player : game.players) {
                if (player->id != game.my_id)
                    ship_count = min(ship_count, player->ships.size());
            }
        }

        if (me->halite >= SHIP_COST &&
            !game_map->is_vis(me->shipyard->position, 1) &&
            (game.turn_number <=
                 MAX_TURNS *
                     spawn_factor[game_map->width][game.players.size()] ||
             me->ships.size() < ship_count)) {
            command_queue.push_back(me->shipyard->spawn());
        }

        auto end = steady_clock::now();
        log::log("Millis:", duration_cast<milliseconds>(end - begin).count());

        if (!game.end_turn(command_queue)) break;
    }
}
