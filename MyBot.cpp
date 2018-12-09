#include "MyBot.h"
#include "hlt/game.hpp"
#include "hungarian/Hungarian.h"

#include <bits/stdc++.h>

using namespace std;
using namespace hlt;
using namespace constants;
using namespace chrono;

template <typename V>
using position_map = unordered_map<Position, V>;

Game game;
unordered_map<EntityId, Task> tasks;

// Fluorine JSON.
stringstream flog;
void message(Position p, string c) {
    flog << "{\"t\": " << game.turn_number << ", \"x\": " << p.x
         << ", \"y\": " << p.y << ", \"color\": \"" << c << "\"}," << endl;
}

inline bool hard_stuck(shared_ptr<Ship> ship) {
    const Halite left = game.game_map->at(ship)->halite;
    return ship->halite < left / MOVE_COST_RATIO;
}

inline bool safe_to_move(shared_ptr<Ship> ship, Position p) {
    unique_ptr<GameMap>& game_map = game.game_map;

    MapCell* cell = game_map->at(p);
    if (!cell->is_occupied()) return true;

    bool safe = game.players.size() != 4;
    safe &= ship->owner != cell->ship->owner;
    safe &= tasks[ship->id] == EXPLORE;
    safe &= ship->halite + MAX_HALITE / 2 <= cell->ship->halite;

    if (!safe) return false;

    // Estimate who is closer.
    double votes = 0.0;
    for (auto& it : game.me->ships) {
        double d = game_map->calculate_distance(p, it.second->position);
        votes -= d * d / game.me->ships.size();
    }
    for (auto& player : game.players) {
        if (player->id != cell->ship->owner) continue;
        for (auto& it : player->ships) {
            double d = game_map->calculate_distance(p, it.second->position);
            votes += d * d / player->ships.size();
        }
    }
    return votes >= 0;
}

template <typename F>
void bfs(position_map<Halite>& dist, vector<Position>& sources, F f) {
    for (vector<MapCell>& cell_row : game.game_map->cells)
        for (MapCell& map_cell : cell_row) dist[map_cell.position] = -1;

    queue<Position> q;
    position_map<bool> vis;
    for (Position p : sources) {
        q.push(p);
        dist[p] = 0;
    }

    while (!q.empty()) {
        Position p = q.front();
        q.pop();
        if (vis[p]) continue;
        vis[p] = true;

        const Halite cost = game.game_map->at(p)->halite / MOVE_COST_RATIO;
        for (Position pp : p.get_surrounding_cardinals()) {
            pp = game.game_map->normalize(pp);
            if (vis[pp]) continue;
            if (dist[pp] == -1 || f(dist[p] + cost, dist[pp])) {
                dist[pp] = dist[p] + cost;
                q.push(pp);
            }
        }
    }
}

// Navigate to |ship->next|.
pair<Direction, double> random_walk(shared_ptr<Ship> ship) {
    Position p = ship->position;
    Halite ship_halite = ship->halite;
    Halite map_halite = game.game_map->at(ship)->halite;
    Direction first_direction = Direction::UNDEFINED;

    Halite burned_halite = 0;

    double t = 1;
    for (; p != ship->next; ++t) {
        auto moves =
            game.game_map->get_moves(p, ship->next, ship_halite, map_halite);
        Direction d = moves[rand() % moves.size()];
        if (first_direction == Direction::UNDEFINED) first_direction = d;

        if (d == Direction::STILL) {
            Halite mined = (map_halite + EXTRACT_RATIO - 1) / EXTRACT_RATIO;
            mined = min(mined, MAX_HALITE - ship_halite);
            ship_halite += mined;
            if (game.game_map->at(p)->inspired) {
                ship_halite += INSPIRED_BONUS_MULTIPLIER * mined;
                ship_halite = min(ship_halite, MAX_HALITE);
            }
            map_halite -= mined;
        } else {
            const Halite delta = map_halite / MOVE_COST_RATIO;
            ship_halite -= delta;
            p = game.game_map->normalize(p.directional_offset(d));
            map_halite = game.game_map->at(p)->halite;

            burned_halite += delta;
        }

        if (tasks[ship->id] == EXPLORE && ship_halite > MAX_HALITE * 0.95)
            break;
    }

    if (first_direction == Direction::UNDEFINED)
        first_direction = Direction::STILL;
    if (game.turn_number + t > MAX_TURNS) ship_halite = 0;

    return {first_direction, (ship_halite - burned_halite) / t};
}

position_map<double> generate_costs(shared_ptr<Ship> ship) {
    unique_ptr<GameMap>& game_map = game.game_map;
    Position p = ship->position;

    position_map<double> surrounding_cost;

    // Default values.
    for (Position pp : p.get_surrounding_cardinals())
        surrounding_cost[game_map->normalize(pp)] = 1e5;

    // TODO: Try taking 75p or the mean instead of the max.
    // Optimize values with random walks.
    map<Direction, double> best_walk;
    for (size_t i = 0; i < 500; ++i) {
        auto walk = random_walk(ship);
        best_walk[walk.first] = max(best_walk[walk.first], walk.second);
    }
    vector<Direction> d;
    for (auto& it : best_walk) {
        // log::log(ship->position, "->", ship->next, "First Step:",
        // it.first, "Rate:", it.second);
        d.push_back(it.first);
    }
    sort(d.begin(), d.end(),
         [&](Direction u, Direction v) { return best_walk[u] > best_walk[v]; });
    for (size_t i = 0; i < d.size(); ++i) {
        Position pp = game_map->normalize(p.directional_offset(d[i]));
        surrounding_cost[pp] = pow(1e2, i);
    }

    if (tasks[ship->id] != EXPLORE) surrounding_cost[p] = 1e7;

    return surrounding_cost;
}

int main(int argc, char* argv[]) {
    game.ready("HaoHaoBot");

    double spawn_factor;
    {
        unordered_map<int, unordered_map<int, double>> f;

        f[32][2] = 0.35;
        f[40][2] = 0.45;
        f[48][2] = 0.525;
        f[56][2] = 0.55;
        f[64][2] = 0.625;

        f[32][4] = 0.3;
        f[40][4] = 0.375;
        f[48][4] = 0.475;
        f[56][4] = 0.475;
        f[64][4] = 0.5;

        spawn_factor = f[game.game_map->width][game.players.size()];
    }

    bool started_hard_return = false;
    Halite wanted_dropoff = 0;

    for (;;) {
        auto begin = steady_clock::now();

        game.update_frame();
        shared_ptr<Player> me = game.me;
        unique_ptr<GameMap>& game_map = game.game_map;

        position_map<Halite> cost_to_base;

        vector<Command> command_queue;

        log::log("Dropoffs.");
#if 1
        for (auto it = me->ships.begin(); it != me->ships.end();) {
            auto ship = it->second;

            Halite halite_around = 0;
            size_t s = 0;
            for (vector<MapCell>& cells : game_map->cells) {
                for (MapCell cell : cells) {
                    int d = game_map->calculate_distance(ship->position,
                                                         cell.position);
                    if (d <= game_map->width / 8) {
                        halite_around += cell.halite;
                        ++s;
                    }
                }
            }

            const int close = game_map->width / 4;

            bool local_dropoffs = false;
            for (auto& player : game.players) {
                int d = game_map->calculate_distance(
                    ship->position, player->shipyard->position);
                local_dropoffs |= d <= close;
                for (auto& it : player->dropoffs) {
                    d = game_map->calculate_distance(ship->position,
                                                     it.second->position);
                    local_dropoffs |= d <= close;
                }
            }

            const Halite delta =
                DROPOFF_COST - game_map->at(ship)->halite + ship->halite;

            bool ideal_dropoff = halite_around >= s * MAX_HALITE * 0.15;
            ideal_dropoff &= !local_dropoffs;
            ideal_dropoff &= game.turn_number <= MAX_TURNS * spawn_factor;
            ideal_dropoff &= me->ships.size() / (2 + me->dropoffs.size()) >= 5;

            if (ideal_dropoff && delta <= me->halite) {
                me->halite -= max(0, delta);
                command_queue.push_back(ship->make_dropoff());
                game.me->dropoffs[-ship->id] = make_shared<Dropoff>(
                    game.my_id, -ship->id, ship->position.x, ship->position.y);
                wanted_dropoff = 0;

                log::log("Dropoff created at", ship->position);

                me->ships.erase(it++);
            } else {
                if (ideal_dropoff) {
                    wanted_dropoff =
                        wanted_dropoff ? min(wanted_dropoff, delta) : delta;
                }
                ++it;
            }
        }
#endif

        log::log("Inspiration. Closest base.");
        for (vector<MapCell>& cell_row : game_map->cells) {
            for (MapCell& cell : cell_row) {
                Position p = cell.position;

                int close_enemies = 0;
                for (auto& player : game.players) {
                    if (player->id == game.my_id) continue;
                    for (auto& it : player->ships) {
                        Position pp = it.second->position;
                        close_enemies += game_map->calculate_distance(p, pp) <=
                                         INSPIRATION_RADIUS;
                    }
                }
                cell.inspired = close_enemies >= INSPIRATION_SHIP_COUNT;

                cell.closest_base = me->shipyard->position;
                for (auto& it : me->dropoffs) {
                    if (game_map->calculate_distance(p, it.second->position) <
                        game_map->calculate_distance(p, cell.closest_base)) {
                        cell.closest_base = it.second->position;
                    }
                }
            }
        }

        // Approximate cost to base..
        vector<Position> sources = {me->shipyard->position};
        for (auto& it : me->dropoffs) sources.push_back(it.second->position);
        bfs(cost_to_base, sources, [&](double u, double v) { return u < v; });

        // Possible targets.
        set<Position> targets;
        for (vector<MapCell>& cell_row : game_map->cells) {
            for (MapCell& cell : cell_row) {
                if (cell.closest_base != cell.position)
                    targets.insert(cell.position);
            }
        }
        for (auto& player : game.players) {
            if (player->id == me->id) continue;
            for (auto& it : player->ships) {
                Position p = it.second->position;
                MapCell* cell = game_map->at(p);

                if (game_map->calculate_distance(p, cell->closest_base) <= 1)
                    continue;

                if (game.players.size() == 4) targets.erase(p);
                cell->mark_unsafe(it.second);

                if (hard_stuck(it.second)) continue;

                for (Position pp : p.get_surrounding_cardinals()) {
                    if (game.players.size() == 4)
                        targets.erase(game_map->normalize(pp));
                    game_map->at(pp)->mark_unsafe(it.second);
                }
            }
        }

        log::log("Tasks.");
        vector<shared_ptr<Ship>> returners, explorers;

        bool all_empty = true;
        for (vector<MapCell>& cell_row : game_map->cells)
            for (MapCell& map_cell : cell_row) all_empty &= !map_cell.halite;

        for (auto& it : me->ships) {
            shared_ptr<Ship> ship = it.second;
            const EntityId id = ship->id;

            MapCell* cell = game_map->at(ship);

            int closest_base_dist = game_map->calculate_distance(
                ship->position, cell->closest_base);

            // New ship.
            if (!tasks.count(id)) tasks[id] = EXPLORE;

            int return_estimate =
                game.turn_number + closest_base_dist + me->ships.size() * 0.3;
            // TODO: Dry run of return.
            if (all_empty || return_estimate >= MAX_TURNS) {
                tasks[id] = HARD_RETURN;
                started_hard_return = true;
            }

            switch (tasks[id]) {
                case EXPLORE:
                    if (ship->halite > MAX_HALITE * 0.95) tasks[id] = RETURN;
                    break;
                case RETURN:
                    if (!closest_base_dist) tasks[id] = EXPLORE;
                case HARD_RETURN:
                    break;
            }

            if (hard_stuck(ship)) {
                command_queue.push_back(ship->stay_still());
                targets.erase(ship->position);
                game_map->at(ship)->mark_unsafe(ship);
                continue;
            }

            // Hard return.
            if (tasks[id] == HARD_RETURN && closest_base_dist <= 1) {
                for (Direction d : ALL_CARDINALS) {
                    if (ship->position.directional_offset(d) ==
                        cell->closest_base) {
                        command_queue.push_back(ship->move(d));
                    }
                }
                continue;
            }

            switch (tasks[id]) {
                case EXPLORE:
                    explorers.push_back(ship);
                    break;
                case HARD_RETURN:
                    if (ship->position == cell->closest_base) break;
                case RETURN:
                    ship->next = cell->closest_base;
                    returners.push_back(ship);
            }
        }

        log::log("Explorer cost matrix.");
        if (!explorers.empty()) {
            vector<vector<double>> cost_matrix;
            for (auto& ship : explorers) {
                position_map<Halite> dist;
                vector<Position> source = {ship->position};
                bfs(dist, source, [&](double u, double v) { return u < v; });

                vector<double> cost;
                for (Position p : targets) {
                    MapCell* cell = game_map->at(p);

                    double d = game_map->calculate_distance(ship->position, p);
                    double dd = sqrt(
                        game_map->calculate_distance(p, cell->closest_base));

                    Halite profit = cell->halite + dist[p];
                    if (cell->inspired)
                        profit += INSPIRED_BONUS_MULTIPLIER * cell->halite;
                    profit = min(profit, MAX_HALITE - ship->halite) -
                             cost_to_base[p];

                    double rate = profit / max(1.0, d + dd);

                    // TODO: Fix.
                    cost.push_back(-rate + 5e3);
                }
                cost_matrix.push_back(move(cost));
            }

            vector<int> assignment(explorers.size());
            HungarianAlgorithm ha;
            ha.Solve(cost_matrix, assignment);

            for (size_t i = 0; i < explorers.size(); ++i) {
                auto it = targets.begin();
                advance(it, assignment[i]);
                explorers[i]->next = *it;
                message(explorers[i]->next, "green");
                // log::log("ID:", explorers[i]->id, "LIVES:",
                // explorers[i]->position, "GOAL:", explorers[i]->next);
            }
        }

        log::log("Move cost matrix.");
        if (!explorers.empty() || !returners.empty()) {
            explorers.insert(explorers.end(), returners.begin(),
                             returners.end());

            set<Position> local_targets;
            for (auto ship : explorers) {
                Position p = ship->position;
                local_targets.insert(p);
                for (Position pp : p.get_surrounding_cardinals())
                    local_targets.insert(game_map->normalize(pp));
            }

            vector<Position> move_space(local_targets.begin(),
                                        local_targets.end());

            position_map<int> move_indices;
            for (size_t i = 0; i < move_space.size(); ++i) {
                move_indices[move_space[i]] = i;
            }

            // Fill cost matrix. Moves in the optimal direction have low
            // cost.
            vector<vector<double>> cost_matrix;
            for (auto ship : explorers) {
                vector<double> cost(move_space.size(), 1e9);

                position_map<double> surrounding_cost = generate_costs(ship);
                for (auto& it : surrounding_cost) {
                    if (safe_to_move(ship, it.first))
                        cost[move_indices[it.first]] = it.second;
                }

                cost_matrix.push_back(move(cost));
            }

            // Solve and execute moves.
            vector<int> assignment(explorers.size());
            HungarianAlgorithm ha;
            ha.Solve(cost_matrix, assignment);

            for (size_t i = 0; i < assignment.size(); ++i) {
                if (explorers[i]->position == move_space[assignment[i]]) {
                    game_map->at(explorers[i])->mark_unsafe(explorers[i]);
                    command_queue.push_back(explorers[i]->stay_still());
                }
                for (Direction d : ALL_CARDINALS) {
                    Position pp = game_map->normalize(
                        explorers[i]->position.directional_offset(d));
                    if (pp == move_space[assignment[i]]) {
                        command_queue.push_back(explorers[i]->move(d));
                        game_map->at(pp)->mark_unsafe(explorers[i]);
                        break;
                    }
                }
            }
        }

        log::log("Spawn ships.");
        size_t ship_lo = 0, ship_hi = numeric_limits<short>::max();
        // TODO: Smarter counter of mid-game aggression.
        if (!started_hard_return) {
            ship_hi = 0;
            for (auto& player : game.players) {
                if (player->id == game.my_id) continue;
                ship_hi = max(ship_hi, player->ships.size());
                ship_lo += player->ships.size();
            }
            ship_lo /= (game.players.size() - 1);
        }

        bool should_spawn = me->halite >= SHIP_COST;
        should_spawn &= !game_map->at(me->shipyard)->is_occupied();
        should_spawn &= !started_hard_return;

        should_spawn &= game.turn_number <= MAX_TURNS * spawn_factor ||
                        me->ships.size() < ship_lo;
        should_spawn &= me->ships.size() <= ship_hi + 5;
        should_spawn &= me->halite >= SHIP_COST + wanted_dropoff;

        if (should_spawn) command_queue.push_back(me->shipyard->spawn());

        if (game.turn_number == MAX_TURNS && game.my_id == 0) {
            log::log("Done!");
            ofstream fout;
            fout.open("replays/__flog.json");
            fout << "[\n" << flog.str();
            fout.close();
        }

        if (!game.end_turn(command_queue)) break;

        auto end = steady_clock::now();
        log::log("Millis: ", duration_cast<milliseconds>(end - begin).count());
    }
}
