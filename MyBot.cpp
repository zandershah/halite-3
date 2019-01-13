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

size_t PADDING = 25;

const double ALPHA = 0.35;
double ewma = MAX_HALITE;
bool should_spawn_ewma = true;

double halite_percentage = 0.0;

bool started_hard_return = false;

unordered_map<EntityId, int> last_moved;

inline Halite extracted(Halite h) {
    return (h + EXTRACT_RATIO - 1) / EXTRACT_RATIO;
}

shared_ptr<Dropoff> future_dropoff;

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

position_map<int> safe_to_move_cache;
enum SafeState { SAFE, ACCEPTABLE, UNSAFE };
SafeState safe_to_move(shared_ptr<Ship> ship, Position p) {
    unique_ptr<GameMap>& game_map = game.game_map;
    MapCell* cell = game_map->at(p);

    if (!cell->is_occupied()) return SAFE;

    if (ship->owner == cell->ship->owner) return UNSAFE;
    if (cell->has_structure() && cell->structure->owner != ship->owner)
        return UNSAFE;
    if (tasks[ship->id] == HARD_RETURN) return ACCEPTABLE;

    // Estimate who is closer.
    if (!safe_to_move_cache.count(p)) {
        int closeness = 0;
        for (auto& it : game.me->ships) {
            if (tasks[it.second->id] != EXPLORE) continue;
            int d = game_map->calc_dist(p, it.second->position) + 1;
            closeness += pow(2, 3 - d);
        }
        for (auto& it : game.players[cell->ship->owner]->ships) {
            if (it.second->id == cell->ship->id) continue;
            int d = game_map->calc_dist(p, it.second->position);
            closeness -= pow(2, 3 - d);
        }
        safe_to_move_cache[p] = closeness;
    }

    const int d = game_map->calc_dist(p, ship->position);
    const int closeness = safe_to_move_cache[p] - pow(2, 2 - d);

    Halite dropped = ship->halite + cell->ship->halite + cell->halite;
    if (cell->inspired) dropped += INSPIRED_BONUS_MULTIPLIER * dropped;
    if (closeness >= 0) {
        if (dropped >= SHIP_COST && ship->halite < cell->ship->halite)
            return SAFE;
        else
            return ACCEPTABLE;
    }
    return UNSAFE;
}

void bfs(position_map<Halite>& dist, Position source) {
    unique_ptr<GameMap>& game_map = game.game_map;

    dist.clear();
    position_map<bool> vis;

    queue<Position> q;
    q.push(source);
    dist[source] = 0;
    while (!q.empty()) {
        Position p = q.front();
        q.pop();

        const Halite cost = game_map->at(p)->halite / MOVE_COST_RATIO;
        for (Position pp : p.get_surrounding_cardinals()) {
            pp = game_map->normalize(pp);
            if (game_map->calc_dist(source, pp) <=
                game_map->calc_dist(source, p)) {
                continue;
            }
            if (game_map->at(pp)->is_occupied()) continue;

            if (!dist.count(pp) || dist[pp] > dist[p] + cost)
                dist[pp] = dist[p] + cost;

            if (!vis[pp]) {
                q.push(pp);
                vis[pp] = true;
            }
        }
    }
}

struct WalkState {
    WalkState(shared_ptr<Ship> ship)
        : ship_id(ship->id),
          p(ship->position),
          starting_ship_halite(ship->halite),
          ship_halite(ship->halite),
          map_halite(game.game_map->at(ship)->halite) {}

    EntityId ship_id;
    Position p;
    Halite starting_ship_halite, ship_halite, map_halite, burned_halite = 0;
    double turns = 1;
    vector<Direction> walk;

    void mine() {
        Halite mined = extracted(map_halite);
        mined = min(mined, MAX_HALITE - ship_halite);
        ship_halite += mined;
        if (game.game_map->at(p)->inspired) {
            ship_halite += INSPIRED_BONUS_MULTIPLIER * mined;
            ship_halite = min(ship_halite, MAX_HALITE);
        }
        map_halite -= mined;
    }

    void move(Direction d) {
        ++turns;
        if (d == Direction::STILL) {
            mine();
            return;
        }
        Halite burned = map_halite / MOVE_COST_RATIO;
        ship_halite -= burned;
        p = game.game_map->normalize(p.doff(d));
        map_halite = game.game_map->at(p)->halite;
        burned_halite += burned;
    }

    double evaluate() const {
        const Halite h = ship_halite - burned_halite;
        double rate;
        if (tasks[ship_id] == EXPLORE) {
            rate = (h - starting_ship_halite) / turns;
        } else {
            rate = h / pow(turns, 2);
        }
        return rate;
    }
};

WalkState random_walk(shared_ptr<Ship> ship, Position d) {
    unique_ptr<GameMap>& game_map = game.game_map;

    WalkState ws(ship);

    double turns = 1;
    for (; ws.p != d && turns <= 50; ++turns) {
        auto moves =
            game_map->get_moves(ws.p, d, ws.ship_halite, ws.map_halite);

        auto rit = remove_if(moves.begin(), moves.end(), [&](Direction d) {
            return safe_to_move(ship, ws.p.doff(d)) == UNSAFE;
        });
        moves.erase(rit, moves.end());
        if (moves.empty()) {
            // We try to add all moves.
            for (Direction d : ALL_CARDINALS) {
                if (safe_to_move(ship, ws.p.doff(d)) != UNSAFE)
                    moves.push_back(d);
            }
        }
        if (moves.empty()) break;

        Direction d = moves[rand() % moves.size()];
        ws.walk.push_back(d);

        ws.move(d);

        if (tasks[ship->id] == EXPLORE && ws.ship_halite > HALITE_RETURN) break;
    }

    if (game.turn_number + turns > MAX_TURNS) ws.ship_halite = 0;

    // Final mine.
    for (size_t i = 0; i < 10; ++i) {
        WalkState ws_copy = ws;
        ws_copy.move(Direction::STILL);
        if (max(0.0, ws.evaluate()) >= ws_copy.evaluate()) break;
        ws = move(ws_copy);
    }

    if (ws.walk.empty()) ws.walk.push_back(Direction::STILL);
    return ws;
}

position_map<int> ideal_dropoff_cache;
Halite ideal_dropoff(Position p) {
    unique_ptr<GameMap>& game_map = game.game_map;

    const int close =
        max(15, game_map->width / (game.players.size() == 2 ? 2 : 3));
    bool local_dropoffs = game_map->at(p)->has_structure();
    local_dropoffs |=
        game_map->calc_dist(p, game.me->shipyard->position) <= close;
    for (auto& it : game.me->dropoffs)
        local_dropoffs |= game_map->calc_dist(p, it.second->position) <= close;

    Halite halite_around = 0;
    double s = 0;
    for (int dy = -5; dy <= 5; ++dy) {
        for (int dx = -5; dx <= 5; ++dx) {
            if (abs(dx) + abs(dy) > 5) continue;
            Position pd(p.x + dx, p.y + dy);

            ++s;

            if (!ideal_dropoff_cache.count(pd)) {
                int ally = 1e3, enemy = 1e3;
                for (auto player : game.players) {
                    for (auto it : player->ships) {
                        int d = game_map->calc_dist(pd, it.second->position);
                        if (player->id == game.my_id)
                            ally = min(ally, d);
                        else
                            enemy = min(enemy, d);
                    }
                }
                ideal_dropoff_cache[pd] = ally - enemy;
            }

            if (ideal_dropoff_cache[pd] <= 1)
                halite_around += game_map->at(pd)->halite;
        }
    }

    Halite saved = halite_around;

    bool ideal = saved >= DROPOFF_COST - game_map->at(p)->halite;
    ideal &= !local_dropoffs;
    ideal &= game.turn_number <= MAX_TURNS - 50;
    ideal &= !started_hard_return;
    // ideal &= local_ships >= 3;

    double bases = 2.0 + game.me->dropoffs.size();
    ideal &= game.me->ships.size() / bases >= 7;

    return ideal * saved;
}

int main(int argc, char* argv[]) {
    game.ready("HaoHaoBot");

    HALITE_RETURN = MAX_HALITE * 0.95;

    Halite total_halite = 0;
    for (const vector<MapCell>& cells : game.game_map->cells)
        for (const MapCell& cell : cells) total_halite += cell.halite;

    unordered_map<EntityId, Halite> last_halite;

    Halite wanted = 0;

    for (;;) {
        game.update_frame();
        shared_ptr<Player> me = game.me;
        unique_ptr<GameMap>& game_map = game.game_map;
        auto begin = steady_clock::now();

        safe_to_move_cache.clear();
        ideal_dropoff_cache.clear();
        if (me->ships.size() >= 75 || game_map->width >= 56) PADDING = 15;
        if (me->ships.size() >= 100 && game_map->width == 64) PADDING = 5;

        vector<Command> command_queue;

        log::log("Dropoffs.");
        if (future_dropoff && !ideal_dropoff(future_dropoff->position)) {
            future_dropoff = nullptr;
            wanted = 0;
        }
        if (future_dropoff) {
            shared_ptr<Ship> ship = nullptr;
            for (auto& it : me->ships) {
                if (it.second->position == future_dropoff->position)
                    ship = it.second;
            }
            if (ship &&
                DROPOFF_COST - game_map->at(ship)->halite - ship->halite <=
                    me->halite) {
                me->halite -= max(0, DROPOFF_COST - game_map->at(ship)->halite -
                                         ship->halite);
                command_queue.push_back(ship->make_dropoff());
                me->dropoffs[-ship->id] = make_shared<Dropoff>(
                    game.my_id, -ship->id, ship->position.x, ship->position.y);
                log::log("Dropoff created at", ship->position);
                wanted = 0;
                future_dropoff = nullptr;

                me->ships.erase(ship->id);
            } else {
                me->dropoffs[future_dropoff->id] = future_dropoff;
                message(future_dropoff->position, "red");
            }
        }

        auto end = steady_clock::now();
        log::log("Millis: ", duration_cast<milliseconds>(end - begin).count());

        log::log("Inspiration. Closest base.");
        position_map<int> close_enemies;
        for (auto& player : game.players) {
            const int IR = INSPIRATION_RADIUS;
            if (player->id == game.my_id) continue;
            for (auto& it : player->ships) {
                Position p = it.second->position;
                for (int dx = -IR; dx <= IR; ++dx) {
                    for (int dy = -IR; dy <= IR; ++dy) {
                        if (abs(dx) + abs(dy) > IR) continue;
                        ++close_enemies[game_map->normalize(
                            Position(p.x + dx, p.y + dy))];
                    }
                }
            }
        }

        Halite current_halite = 0;
        bool all_empty = true;
        set<Position> targets;
        for (vector<MapCell>& cell_row : game_map->cells) {
            for (MapCell& cell : cell_row) {
                Position p = cell.position;

                cell.inspired = close_enemies[p] >= INSPIRATION_SHIP_COUNT;
                cell.close_ships.clear();
                cell.closest_base = me->shipyard->position;
                for (auto& it : me->dropoffs) {
                    if (game_map->calc_dist(p, it.second->position) <
                        game_map->calc_dist(p, cell.closest_base)) {
                        cell.closest_base = it.second->position;
                    }
                }

                current_halite += cell.halite;
                all_empty &= !cell.halite;
                targets.insert(cell.position);
            }
        }
        halite_percentage = current_halite * 1.0 / total_halite;

        for (auto& it : me->ships) {
            MapCell* cell = game_map->at(it.second);
            auto moves = game_map->get_moves(cell->position, cell->closest_base,
                                             it.second->halite, 0);
            if (moves.empty()) continue;

            Direction od = moves.front();
            for (Direction d : moves)
                if (cell->close_ships[d] < cell->close_ships[od]) od = d;
            ++cell->close_ships[od];
        }

        for (auto& player : game.players) {
            if (player->id == me->id) continue;
            for (auto& it : player->ships) {
                auto ship = it.second;
                Position p = ship->position;
                MapCell* cell = game_map->at(p);

                if (game_map->calc_dist(p, cell->closest_base) <= 1) continue;

                cell->mark_unsafe(ship);
                if (hard_stuck(ship)) continue;

                for (Position pp : p.get_surrounding_cardinals())
                    game_map->at(pp)->mark_unsafe(ship);
            }
        }

        // Hard return.
        for (auto& it : me->ships) {
            shared_ptr<Ship> ship = it.second;
            const EntityId id = ship->id;

            MapCell* cell = game_map->at(ship);

            if (!tasks.count(id)) continue;

            double return_turn = MAX_TURNS;

            const Task task_holder = tasks[id];
            tasks[id] = HARD_RETURN;
            for (size_t i = 0; i < 50; ++i) {
                auto ws = random_walk(it.second, cell->closest_base);
                return_turn = min(return_turn, ws.turns);
            }
            tasks[id] = task_holder;

            auto moves = game_map->get_moves(cell->position, cell->closest_base,
                                             it.second->halite, 0);
            if (moves.empty()) continue;

            Direction od = moves.front();
            for (Direction d : moves)
                if (cell->close_ships[d] < cell->close_ships[od]) od = d;
            return_turn = max(return_turn, 1.0 * cell->close_ships[od]);

            return_turn += game.turn_number;
            if (all_empty || return_turn > MAX_TURNS) {
                tasks[id] = HARD_RETURN;
                started_hard_return = true;
            }
        }

        log::log("Tasks.");
        vector<shared_ptr<Ship>> returners, explorers;
        for (auto& it : me->ships) {
            shared_ptr<Ship> ship = it.second;
            const EntityId id = ship->id;

            MapCell* cell = game_map->at(ship);

            int closest_base_dist =
                game_map->calc_dist(ship->position, cell->closest_base);

            // New ship.
            if (!tasks.count(id)) tasks[id] = EXPLORE;

            // Return if game will end soon.
            if (started_hard_return) tasks[id] = HARD_RETURN;

            switch (tasks[id]) {
                case EXPLORE:
                    if (ship->halite > min(HALITE_RETURN, current_halite * 1.0))
                        tasks[id] = RETURN;
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
                case EXPLORE:
                    explorers.push_back(ship);
                    break;
                case HARD_RETURN:
                    if (ship->position == cell->closest_base) break;
                case RETURN:
                    if (future_dropoff) {
                        int d_close =
                            game_map->calc_dist(cell->closest_base, ship->next);
                        int d_new = game_map->calc_dist(
                            future_dropoff->position, ship->next);
                        int d_apart = game_map->calc_dist(
                            cell->closest_base, future_dropoff->position);
                        if (d_close > 5 && d_new < d_apart) {
                            ship->next = future_dropoff->position;
                        } else {
                            ship->next = cell->closest_base;
                        }
                    } else {
                        ship->next = cell->closest_base;
                    }
                    returners.push_back(ship);
            }
        }

        end = steady_clock::now();
        log::log("Millis: ", duration_cast<milliseconds>(end - begin).count());

        log::log("Explorer cost matrix.");
        {
            vector<vector<double>> uncompressed_cost_matrix;
            uncompressed_cost_matrix.reserve(explorers.size());
            vector<bool> is_top_target(targets.size());
            vector<double> top_score;
            top_score.reserve(explorers.size());

            for (auto it = explorers.begin(); it != explorers.end();) {
                auto ship = *it;

                position_map<Halite> dist;
                bfs(dist, ship->position);
                priority_queue<double> pq;

                vector<double> uncompressed_cost;
                uncompressed_cost.reserve(targets.size());
                for (Position p : targets) {
                    MapCell* cell = game_map->at(p);

                    double d = game_map->calc_dist(ship->position, p);
                    double dd = game_map->calc_dist(p, cell->closest_base);

                    Halite profit = cell->halite - dist[p];

                    bool should_inspire =
                        game.players.size() == 4 || d <= INSPIRATION_RADIUS;
                    if (cell->inspired && should_inspire)
                        profit += INSPIRED_BONUS_MULTIPLIER * cell->halite;
                    if (d <= 1 && cell->ship &&
                        cell->ship->owner != game.my_id &&
                        hard_stuck(cell->ship)) {
                        profit += (INSPIRED_BONUS_MULTIPLIER + 1) *
                                  cell->ship->halite;
                    }
                    if (future_dropoff &&
                        game_map->calc_dist(future_dropoff->position, p) <= 3) {
                        profit += INSPIRED_BONUS_MULTIPLIER * cell->halite;
                    }
                    profit = min(profit, MAX_HALITE - ship->halite);

                    if (safe_to_move(ship, p) == UNSAFE) profit = 0;
                    double rate = profit / (1.0 + d + dd);

                    uncompressed_cost.push_back(-rate + 5e3);
                    if (rate > 0) pq.push(uncompressed_cost.back());
                    while (pq.size() > PADDING) pq.pop();
                }

                if (pq.empty()) {
                    log::log("Skipping exploration for", ship->id);
                    tasks[ship->id] = RETURN;
                    ship->next = game_map->at(ship)->closest_base;
                    returners.push_back(ship);
                    it = explorers.erase(it);
                    continue;
                }

                for (size_t i = 0; i < uncompressed_cost.size(); ++i) {
                    if (!is_top_target[i] && uncompressed_cost[i] <= pq.top())
                        is_top_target[i] = true;
                }
                top_score.push_back(pq.top());
                uncompressed_cost_matrix.push_back(move(uncompressed_cost));

                ++it;
            }

            if (!explorers.empty()) {
                // Coordinate compress.
                vector<Position> target_space;
                {
                    auto it = targets.begin();
                    for (size_t i = 0; i < targets.size(); ++i, ++it) {
                        if (is_top_target[i]) {
                            target_space.push_back(*it);
                            // message(*it, "blue");
                        }
                    }
                }

                log::log("Compressed space:", target_space.size());
                end = steady_clock::now();
                log::log("Millis: ",
                         duration_cast<milliseconds>(end - begin).count());

                vector<vector<double>> cost_matrix;
                cost_matrix.reserve(explorers.size());
                for (size_t i = 0; i < explorers.size(); ++i) {
                    const vector<double>& uncompressed_cost =
                        uncompressed_cost_matrix[i];
                    vector<double> cost;
                    cost.reserve(target_space.size());
                    for (size_t j = 0; j < uncompressed_cost.size(); ++j) {
                        if (!is_top_target[j]) continue;
                        cost.push_back(uncompressed_cost[j]);
                    }
                    cost_matrix.push_back(move(cost));
                }

                end = steady_clock::now();
                log::log("Millis: ",
                         duration_cast<milliseconds>(end - begin).count());

                vector<int> assignment(explorers.size());
                HungarianAlgorithm ha;
                ha.Solve(cost_matrix, assignment);

                end = steady_clock::now();
                log::log("Millis: ",
                         duration_cast<milliseconds>(end - begin).count());

                for (size_t i = 0; i < explorers.size(); ++i) {
                    explorers[i]->next = target_space[assignment[i]];
                    // message(explorers[i]->next, "green");
                }
            }

            end = steady_clock::now();
            log::log("Millis: ",
                     duration_cast<milliseconds>(end - begin).count());
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

            // Coordinate compress.
            position_map<int> move_indices;
            for (size_t i = 0; i < move_space.size(); ++i)
                move_indices[move_space[i]] = i;

            // Fill cost matrix. Optimal direction has low cost.
            vector<vector<double>> cost_matrix;
            cost_matrix.reserve(explorers.size());
            for (auto ship : explorers) {
                vector<double> cost(move_space.size(), 1e9);
                cost_matrix.push_back(move(cost));
            }

            // Random walk to generate costs.
            vector<unordered_map<Direction, double>> best_walks(
                explorers.size());
            bool timeout = false;
            while (!timeout) {
                for (size_t i = 0; i < explorers.size() && !timeout; ++i) {
                    if (duration_cast<milliseconds>(steady_clock::now() - end)
                            .count() > 50) {
                        timeout = true;
                    }
                    if (duration_cast<milliseconds>(steady_clock::now() - begin)
                            .count() > 1750) {
                        timeout = true;
                    }

                    auto ws = random_walk(explorers[i], explorers[i]->next);
                    best_walks[i][ws.walk.front()] =
                        max(best_walks[i][ws.walk.front()], ws.evaluate());
                }
            }

            for (size_t i = 0; i < explorers.size(); ++i) {
                Position p = explorers[i]->position;

                position_map<double> surrounding_cost;

                // Default values.
                for (Position pp : p.get_surrounding_cardinals())
                    surrounding_cost[game_map->normalize(pp)] = 1e5;

                if (p == explorers[i]->next) {
                    surrounding_cost[p] = 1;
                } else {
                    double best = 1.0;
                    for (auto& it : best_walks[i]) best = max(best, it.second);
                    for (auto& it : best_walks[i]) {
                        Position pp = game_map->normalize(p.doff(it.first));
                        surrounding_cost[pp] = pow(1e3, 1.0 - it.second / best);
                    }

                    if (last_moved[explorers[i]->id] <= game.turn_number - 5)
                        surrounding_cost[p] = 1e7;
                }

                for (auto& it : surrounding_cost) {
                    SafeState safe_state = safe_to_move(explorers[i], it.first);
                    if (safe_state == SAFE)
                        cost_matrix[i][move_indices[it.first]] = it.second;
                    else if (safe_state == ACCEPTABLE)
                        cost_matrix[i][move_indices[it.first]] = it.second * 5;
                    else
                        cost_matrix[i][move_indices[it.first]] = 1e9;
                }
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
                    Position pp =
                        game_map->normalize(explorers[i]->position.doff(d));
                    if (pp == move_space[assignment[i]]) {
                        command_queue.push_back(explorers[i]->move(d));
                        game_map->at(pp)->mark_unsafe(explorers[i]);
                        last_moved[explorers[i]->id] = game.turn_number;
                        break;
                    }
                }
            }
        }

        vector<pair<Position, double>> futures;
        for (Position p : targets) {
            Halite ideal = ideal_dropoff(p);
            if (!ideal) continue;

            double d = 1;
            for (auto it : me->ships)
                d += pow(game_map->calc_dist(p, it.second->position), 2);
            d /= me->ships.size();

            futures.emplace_back(p, ideal / d);
        }
        sort(futures.begin(), futures.end(),
             [&](pair<Position, double> u, pair<Position, double> v) {
                 return u.second > v.second;
             });
        if (!futures.empty() && !future_dropoff) {
            wanted = DROPOFF_COST -
                     game_map->at(futures.front().first)->halite -
                     HALITE_RETURN * 0.75;
            if (wanted <= me->halite) {
                // message(futures.front().first, "green");
                future_dropoff = make_shared<Dropoff>(game.my_id, -2,
                                                      futures.front().first.x,
                                                      futures.front().first.y);
            }
        }

        end = steady_clock::now();
        log::log("Millis: ", duration_cast<milliseconds>(end - begin).count());

        if (game.turn_number % 5 == 0) {
            Halite h = 0;
            for (auto ship : explorers) {
                if (ship->halite >= last_halite[ship->id])
                    h += ship->halite - last_halite[ship->id];
                last_halite[ship->id] = ship->halite;
            }
            ewma = ALPHA * h / (me->ships.size() * 5) + (1 - ALPHA) * ewma;

            int total_ships = 0;
            for (auto player : game.players)
                total_ships += player->ships.size();

            double gather_turns = 2 * SHIP_COST / ewma;
            should_spawn_ewma =
                game.turn_number + gather_turns < MAX_TURNS - 50 &&
                current_halite / total_ships > 2 * SHIP_COST;

            log::log("EWMA:", ewma, "Should spawn ships:", should_spawn_ewma);
        }

        log::log("Spawn ships.");
        size_t ship_lo = 0, ship_hi = 1e3;
        if (!started_hard_return) {
            swap(ship_lo, ship_hi);
            for (auto& player : game.players) {
                if (player->id == game.my_id) continue;
                ship_lo = min(ship_lo, player->ships.size());
                ship_hi = max(ship_hi, player->ships.size());
            }
        }

        bool should_spawn = me->halite >= SHIP_COST + wanted;
        should_spawn &= !game_map->at(me->shipyard)->is_occupied();
        should_spawn &= !started_hard_return;

        should_spawn &= should_spawn_ewma || me->ships.size() < ship_lo;
        should_spawn &= me->ships.size() < ship_hi + 3;
        should_spawn &= halite_percentage >= 0.1;

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
