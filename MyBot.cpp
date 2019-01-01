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

double HALITE_RETURN;

const double ALPHA = 0.35;
double ewma = MAX_HALITE;
bool should_spawn_ewma = true;

bool started_hard_return = false;

unordered_map<EntityId, int> last_moved;

inline Halite extracted(Halite h) {
    return (h + EXTRACT_RATIO - 1) / EXTRACT_RATIO;
}

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

bool safe_to_move(shared_ptr<Ship> ship, Position p) {
    unique_ptr<GameMap>& game_map = game.game_map;

    MapCell* cell = game_map->at(p);
    if (!cell->is_occupied()) return true;
    if (ship->owner == cell->ship->owner) return false;
    if (tasks[ship->id] == HARD_RETURN) return true;

    // Estimate who is closer.
    int ally = 0, evil = 0;
    for (auto& it : game.me->ships) {
        if (it.second->id == ship->id || tasks[it.second->id] != EXPLORE)
            continue;
        int d = game_map->calculate_distance(p, it.second->position);
        ally += pow(2, 5 - d);
    }
    for (auto& it : game.players[cell->ship->owner]->ships) {
        if (it.second->id == cell->ship->id) continue;
        int d = game_map->calculate_distance(p, it.second->position);
        evil += pow(2, 5 - d);
    }
    return game.players.size() == 2 && ally > evil;
}

void bfs(position_map<Halite>& dist, Position source) {
    unique_ptr<GameMap>& game_map = game.game_map;

    for (const vector<MapCell>& cell_row : game_map->cells)
        for (const MapCell& map_cell : cell_row) dist[map_cell.position] = 1e6;
    position_map<bool> vis;

    queue<Position> q;
    q.push(source);
    dist[source] = 0;
    while (!q.empty()) {
        Position p = q.front();
        q.pop();

        if (dist[p] > game_map->at(p)->halite + MAX_HALITE / 20) continue;

        const Halite cost = game_map->at(p)->halite / MOVE_COST_RATIO;
        for (Position pp : p.get_surrounding_cardinals()) {
            pp = game_map->normalize(pp);
            if (game_map->calculate_distance(source, pp) <=
                game_map->calculate_distance(source, p)) {
                continue;
            }
            if (game_map->at(pp)->is_occupied()) continue;
            dist[pp] = min(dist[pp], dist[p] + cost);
            if (!vis[pp]) {
                q.push(pp);
                vis[pp] = true;
            }
        }
    }
}

pair<vector<Direction>, double> random_explore(shared_ptr<Ship> ship) {
    unique_ptr<GameMap>& game_map = game.game_map;

    Position p = ship->position;

    Halite ship_halite = ship->halite;
    Halite map_halite = game_map->at(ship)->halite;
    Halite burned_halite = 0;

    vector<Direction> walk;
    set<Position> vis;

    double t = 1;

    for (; t <= 25 && ship_halite < HALITE_RETURN; ++t) {
        auto moves = game_map->get_moves(p, vis, ship_halite, map_halite);

        auto rit = remove_if(moves.begin(), moves.end(), [&](Direction d) {
            return !safe_to_move(ship, p.doff(d));
        });
        moves.erase(rit, moves.end());
        if (moves.empty()) break;

        // Moves are weighted by their halite.
        Halite move_total = 0;
        for (Direction d : moves) {
            Halite h =
                game_map->at(p.doff(d))->halite - map_halite / MOVE_COST_RATIO;
            if (d == Direction::STILL) h = map_halite * 5;
            h *= pow(0.75, game_map->at(p)->claimed);
            h = max(1, h);
            move_total += h;
        }
        Halite move_rand = rand() % move_total;
        Direction move_d = Direction::UNDEFINED;
        for (Direction d : moves) {
            Halite h =
                game_map->at(p.doff(d))->halite - map_halite / MOVE_COST_RATIO;
            if (d == Direction::STILL) h = map_halite * 5;
            h *= pow(0.75, game_map->at(p)->claimed);
            h = max(1, h);
            if (move_rand < h) {
                move_d = d;
                break;
            }
            move_rand -= h;
        }
        assert(move_d != Direction::UNDEFINED);

        walk.push_back(move_d);

        if (move_d == Direction::STILL) {
            Halite mined = extracted(map_halite);
            mined = min(mined, MAX_HALITE - ship_halite);
            ship_halite += mined;
            if (game_map->at(p)->inspired) {
                ship_halite += INSPIRED_BONUS_MULTIPLIER * mined;
                ship_halite = min(ship_halite, MAX_HALITE);
            }
            map_halite -= mined;
        } else {
            const Halite burned = map_halite / MOVE_COST_RATIO;
            ship_halite -= burned;
            burned_halite += burned;

            vis.insert(p);
            p = game_map->normalize(p.doff(move_d));
            map_halite = game_map->at(p)->halite;
        }
    }

    if (t + game_map->calculate_distance(p, game_map->at(p)->closest_base) >
        MAX_TURNS)
        ship_halite = 0;

    if (walk.empty()) walk.push_back(Direction::STILL);
    Halite end_halite = ship_halite - burned_halite - ship->halite;
    return {move(walk), end_halite / t};
}

pair<vector<Direction>, double> random_return(shared_ptr<Ship> ship) {
    unique_ptr<GameMap>& game_map = game.game_map;

    Position p = ship->position;
    Halite ship_halite = ship->halite;
    Halite map_halite = game_map->at(ship)->halite;
    vector<Direction> walk;
    Halite burned_halite = 0;

    double t = 1;

    for (; p != ship->next && t <= 100; ++t) {
        auto moves =
            game_map->get_moves(p, ship->next, ship_halite, map_halite);

        auto rit = remove_if(moves.begin(), moves.end(), [&](Direction d) {
            return !safe_to_move(ship, p.doff(d));
        });
        moves.erase(rit, moves.end());
        if (moves.empty()) break;

        Direction d = moves[rand() % moves.size()];
        walk.push_back(d);

        if (d == Direction::STILL) {
            Halite mined = extracted(map_halite);
            mined = min(mined, MAX_HALITE - ship_halite);
            ship_halite += mined;
            if (game_map->at(p)->inspired) {
                ship_halite += INSPIRED_BONUS_MULTIPLIER * mined;
                ship_halite = min(ship_halite, MAX_HALITE);
            }
            map_halite -= mined;
        } else {
            const Halite burned = map_halite / MOVE_COST_RATIO;
            ship_halite -= burned;
            p = game_map->normalize(p.doff(d));
            map_halite = game_map->at(p)->halite;

            burned_halite += burned;
        }
    }

    burned_halite = 0;
    if (walk.empty()) walk.push_back(Direction::STILL);
    return {move(walk), (ship_halite - burned_halite) / pow(t, 2)};
}

position_map<double> generate_costs(shared_ptr<Ship> ship,
                                    map<Direction, double> walks) {
    unique_ptr<GameMap>& game_map = game.game_map;
    Position p = ship->position;

    position_map<double> surrounding_cost;

    // Default values.
    for (Position pp : p.get_surrounding_cardinals())
        surrounding_cost[game_map->normalize(pp)] = 1e5;

    walks.erase(Direction::UNDEFINED);

    double best = 1.0;
    for (auto& it : walks) best = max(best, it.second);
    for (auto& it : walks) {
        Position pp = game_map->normalize(p.doff(it.first));
        surrounding_cost[pp] = pow(1e3, 1 - it.second / best);
    }

    if (last_moved[ship->id] <= game.turn_number - 5) surrounding_cost[p] = 1e7;
    return surrounding_cost;
}

bool ideal_dropoff(Position p) {
    unique_ptr<GameMap>& game_map = game.game_map;

    const int close = max(15, game_map->width / 3);
    bool local_dropoffs = game_map->at(p)->has_structure();
    local_dropoffs |=
        game_map->calculate_distance(p, game.me->shipyard->position) <= close;
    for (auto& it : game.me->dropoffs) {
        local_dropoffs |=
            game_map->calculate_distance(p, it.second->position) <= close;
    }

    // Approximate number of turns saved mining out.
    Halite halite_around = 0;
    const int CLOSE_MINE = 5;
    for (int dy = -CLOSE_MINE; dy <= CLOSE_MINE; ++dy) {
        for (int dx = -CLOSE_MINE; dx <= CLOSE_MINE; ++dx) {
            if (abs(dx) + abs(dy) > CLOSE_MINE) continue;

            Position pd(p.x + dx, p.y + dy);
            if (game_map->at(pd)->ship) {
                if (game_map->at(pd)->ship->owner != game.my_id) continue;
                halite_around += game_map->at(pd)->ship->halite;
            }

            halite_around += game_map->at(pd)->halite;
        }
    }
    Halite saved =
        halite_around / MAX_HALITE * ewma *
        game_map->calculate_distance(p, game_map->at(p)->closest_base);

    bool ideal = saved >= DROPOFF_COST - game_map->at(p)->halite;
    ideal &= !local_dropoffs;
    ideal &= game.turn_number <= MAX_TURNS - 75;
    ideal &= !started_hard_return;

    double bases = 2.0 + game.me->dropoffs.size();
    ideal &= game.me->ships.size() / bases >= 8;

    return ideal;
}

int main(int argc, char* argv[]) {
    game.ready("HaoHaoBot");

    HALITE_RETURN = MAX_HALITE * 0.95;

    Halite total_halite = 0;
    for (const vector<MapCell>& cells : game.game_map->cells)
        for (const MapCell& cell : cells) total_halite += cell.halite;

    unordered_map<EntityId, Halite> last_halite;
    bool wanted = false;

    for (;;) {
        game.update_frame();
        shared_ptr<Player> me = game.me;
        unique_ptr<GameMap>& game_map = game.game_map;
        auto begin = steady_clock::now();

        vector<Command> command_queue;

        log::log("Dropoffs.");
#if 1
        for (auto it = me->ships.begin(); it != me->ships.end();) {
            auto ship = it->second;

            bool ideal = ideal_dropoff(ship->position);
            const Halite delta =
                DROPOFF_COST - game_map->at(ship)->halite - ship->halite;

            if (ideal && delta <= me->halite) {
                me->halite -= max(0, delta);
                command_queue.push_back(ship->make_dropoff());
                game.me->dropoffs[-ship->id] = make_shared<Dropoff>(
                    game.my_id, -ship->id, ship->position.x, ship->position.y);
                log::log("Dropoff created at", ship->position);
                wanted = false;

                me->ships.erase(it++);
            } else {
                if (ideal) wanted = true;
                ++it;
            }
        }
#endif
        auto end = steady_clock::now();
        log::log("Millis: ", duration_cast<milliseconds>(end - begin).count());

        log::log("Inspiration. Closest base.");
        position_map<int> close_enemies;
        for (auto& player : game.players) {
            if (player->id == game.my_id) continue;
            const int CLOSE = INSPIRATION_RADIUS;
            for (auto& it : player->ships) {
                Position p = it.second->position;
                for (int dx = -CLOSE; dx <= CLOSE; ++dx) {
                    for (int dy = -CLOSE; dy <= CLOSE; ++dy) {
                        if (abs(dx) + abs(dy) > CLOSE) continue;
                        ++close_enemies[game_map->normalize(
                            Position(p.x + dx, p.y + dy))];
                    }
                }
            }
        }
        Halite current_halite = 0;
        for (vector<MapCell>& cell_row : game_map->cells) {
            for (MapCell& cell : cell_row) {
                Position p = cell.position;

                cell.inspired = close_enemies[p] >= INSPIRATION_SHIP_COUNT;
                cell.claimed = 0;
                cell.close_ships = 0;
                cell.closest_base = me->shipyard->position;
                for (auto& it : me->dropoffs) {
                    if (game_map->calculate_distance(p, it.second->position) <
                        game_map->calculate_distance(p, cell.closest_base)) {
                        cell.closest_base = it.second->position;
                    }
                }

                current_halite += cell.halite;
            }
        }
        for (auto& it : me->ships)
            ++game_map->at(game_map->at(it.second)->closest_base)->close_ships;

        set<Position> targets;
        for (const vector<MapCell>& cell_row : game_map->cells)
            for (const MapCell& cell : cell_row) targets.insert(cell.position);

        for (auto& player : game.players) {
            if (player->id == me->id) continue;
            for (auto& it : player->ships) {
                auto ship = it.second;
                Position p = ship->position;
                MapCell* cell = game_map->at(p);

                if (game_map->calculate_distance(p, cell->closest_base) <= 1)
                    continue;

                cell->mark_unsafe(it.second);

                if (hard_stuck(it.second)) continue;

                for (Position pp : p.get_surrounding_cardinals())
                    game_map->at(pp)->mark_unsafe(it.second);
            }
        }

        log::log("Tasks.");
        vector<shared_ptr<Ship>> unmoved_ships;

        bool all_empty = true;
        for (const vector<MapCell>& cell_row : game_map->cells)
            for (const MapCell& cell : cell_row) all_empty &= !cell.halite;

        for (auto& it : me->ships) {
            shared_ptr<Ship> ship = it.second;
            const EntityId id = ship->id;

            MapCell* cell = game_map->at(ship);

            int closest_base_dist = game_map->calculate_distance(
                ship->position, cell->closest_base);

            // New ship.
            if (!tasks.count(id)) tasks[id] = EXPLORE;

            // Return estimate if forced.
            const int forced_return_turn =
                game.turn_number + closest_base_dist +
                game_map->at(cell->closest_base)->close_ships * 0.3;
            // TODO: Dry run of return.
            if (all_empty || forced_return_turn > MAX_TURNS) {
                tasks[id] = HARD_RETURN;
                started_hard_return = true;
            }

            switch (tasks[id]) {
                case EXPLORE:
                    if (ship->halite > HALITE_RETURN) tasks[id] = RETURN;
                    break;
                case RETURN:
                    if (!closest_base_dist) {
                        tasks[id] = EXPLORE;
                        last_halite[id] = 0;
                    }
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
                    if (ship->position.doff(d) == cell->closest_base)
                        command_queue.push_back(ship->move(d));
                }
                continue;
            }

            switch (tasks[id]) {
                case HARD_RETURN:
                    if (ship->position == cell->closest_base) break;
                case RETURN:
                    ship->next = cell->closest_base;
                case EXPLORE:
                    unmoved_ships.push_back(ship);
            }
        }

        end = steady_clock::now();
        log::log("Millis: ", duration_cast<milliseconds>(end - begin).count());

        if (!unmoved_ships.empty()) {
            set<Position> local_targets;
            for (auto ship : unmoved_ships) {
                Position p = ship->position;
                local_targets.insert(p);
                for (Position pp : p.get_surrounding_cardinals())
                    local_targets.insert(game_map->normalize(pp));
            }

            vector<Position> move_space(local_targets.begin(),
                                        local_targets.end());
            // Coordinate compress.
            position_map<int> move_indices;
            for (size_t i = 0; i < move_space.size(); ++i)
                move_indices[move_space[i]] = i;

            // Fill cost matrix. Optimal direction has low cost.
            vector<vector<double>> cost_matrix;
            cost_matrix.reserve(unmoved_ships.size());

            for (auto ship : unmoved_ships) {
                map<Direction, double> best_move_direction;
                double best_move = 0;
                vector<Direction> best_path;
                const size_t MAX_WALKS = tasks[ship->id] == EXPLORE ? 150 : 50;
                for (size_t i = 0; i < MAX_WALKS; ++i) {
                    auto walk = tasks[ship->id] == EXPLORE
                                    ? random_explore(ship)
                                    : random_return(ship);
                    if (walk.first.empty()) continue;

                    best_move_direction[walk.first.front()] = max(
                        best_move_direction[walk.first.front()], walk.second);
                    if (walk.second > best_move) {
                        best_move = walk.second;
                        best_path = move(walk.first);
                    }
                }

                vector<double> cost(move_space.size(), 1e9);
                position_map<double> surrounding_cost =
                    generate_costs(ship, best_move_direction);
                for (auto& it : surrounding_cost) {
                    if (safe_to_move(ship, it.first))
                        cost[move_indices[it.first]] = it.second;
                }
                cost_matrix.push_back(move(cost));

                // command_queue.push_back(ship->move(best_path.front()));
                Position p = ship->position;
                // message(p, "green");
                // game_map->at(p.doff(best_path.front()))->mark_unsafe(ship);

#if 1
                if (tasks[ship->id] == EXPLORE && game.players.size() == 4) {
                    ++game_map->at(p)->claimed;
                    for (Direction d : best_path) {
                        p = p.doff(d);
                        ++game_map->at(p)->claimed;
                        // message(p, "green");
                    }
                }
#endif
            }

            // Solve and execute moves.
            vector<int> assignment(unmoved_ships.size());
            HungarianAlgorithm ha;
            ha.Solve(cost_matrix, assignment);

            for (size_t i = 0; i < assignment.size(); ++i) {
                auto ship = unmoved_ships[i];
                if (ship->position == move_space[assignment[i]]) {
                    game_map->at(ship)->mark_unsafe(ship);
                    command_queue.push_back(ship->stay_still());
                }
                for (Direction d : ALL_CARDINALS) {
                    Position pp = game_map->normalize(ship->position.doff(d));
                    if (pp == move_space[assignment[i]]) {
                        command_queue.push_back(ship->move(d));
                        game_map->at(pp)->mark_unsafe(ship);
                        last_moved[ship->id] = game.turn_number;
                        break;
                    }
                }
            }
        }

        end = steady_clock::now();
        log::log("Millis: ", duration_cast<milliseconds>(end - begin).count());

        if (game.turn_number % 5 == 0) {
            Halite h = 0;
            for (auto ship : unmoved_ships) {
                if (ship->halite >= last_halite[ship->id])
                    h += ship->halite - last_halite[ship->id];
                last_halite[ship->id] = ship->halite;
            }
            ewma = ALPHA * h / (unmoved_ships.size() * 5) + (1 - ALPHA) * ewma;
        }
        should_spawn_ewma =
            game.turn_number + 2 * SHIP_COST / ewma < MAX_TURNS - 75;
        log::log("EWMA:", ewma, "Should spawn ships:", should_spawn_ewma);

        log::log("Spawn ships.");
        size_t ship_lo = 0;
        if (!started_hard_return) {
            ship_lo = 1e3;
            for (auto& player : game.players) {
                if (player->id == game.my_id) continue;
                ship_lo = min(ship_lo, player->ships.size());
            }
        }

        bool should_spawn = me->halite >= SHIP_COST + wanted * DROPOFF_COST;
        should_spawn &= !game_map->at(me->shipyard)->is_occupied();
        should_spawn &= !started_hard_return;

        should_spawn &= should_spawn_ewma || me->ships.size() < ship_lo;
        should_spawn &= current_halite * 1.0 / total_halite >= 0.1;

#if 0
        should_spawn &= me->ships.empty();
#endif

        if (should_spawn) {
            command_queue.push_back(me->shipyard->spawn());
            log::log("Spawning ship!");
        }

        if (game.turn_number == MAX_TURNS && game.my_id == 0) {
            log::log("Done!");
            ofstream fout;
            fout.open("replays/__flog.json");
            fout << "[\n" << flog.str();
            fout.close();
        }

        end = steady_clock::now();
        log::log("Millis: ", duration_cast<milliseconds>(end - begin).count());

        if (!game.end_turn(command_queue)) break;
    }
}
