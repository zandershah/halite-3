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

double halite_percentage = 0.0;

bool started_hard_return = false;

unordered_map<EntityId, int> last_moved;

inline Halite extracted(Halite h) {
    return (h + EXTRACT_RATIO - 1) / EXTRACT_RATIO;
}

queue<shared_ptr<Dropoff>> future_dropoffs;
unordered_map<Position, int> new_dropoffs;

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
    if (cell->has_structure() && cell->structure->owner != ship->owner)
        return false;
    if (tasks[ship->id] == HARD_RETURN) return true;

    // Estimate who is closer.
    int ally = 0, enemy = 0;
    for (auto& it : game.me->ships) {
        if (it.second->id == ship->id || tasks[it.second->id] != EXPLORE)
            continue;
        int d = game_map->calculate_distance(p, it.second->position) + 1;
        ally += pow(1.5, 4 - d);
    }
    for (auto& it : game.players[cell->ship->owner]->ships) {
        if (it.second->id == cell->ship->id) continue;
        int d = game_map->calculate_distance(p, it.second->position);
        enemy += pow(1.5, 4 - d);
    }

    if (game.players.size() == 2) return ally >= enemy;
    return ally >= enemy + 5 &&
           ship->halite + cell->ship->halite + cell->halite >= MAX_HALITE * 0.5;
}

void bfs(position_map<Halite>& dist, Position source) {
    unique_ptr<GameMap>& game_map = game.game_map;

    for (const vector<MapCell>& cell_row : game_map->cells)
        for (const MapCell& map_cell : cell_row) dist[map_cell.position] = -1;

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

            if (dist[pp] == -1 || dist[pp] > dist[p] + cost)
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

    // TODO: Prioritize early halite.
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

        if (ws.walk.empty()) {
            auto rit = remove_if(moves.begin(), moves.end(), [&](Direction d) {
                return !safe_to_move(ship, ws.p.doff(d));
            });
            moves.erase(rit, moves.end());
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

Halite ideal_dropoff(Position p) {
    unique_ptr<GameMap>& game_map = game.game_map;

    const int close = game_map->width / 3;
    bool local_dropoffs = game_map->at(p)->has_structure();
    local_dropoffs |=
        game_map->calculate_distance(p, game.me->shipyard->position) <= close;
    for (auto& it : game.me->dropoffs) {
        local_dropoffs |=
            game_map->calculate_distance(p, it.second->position) <= close;
    }

    size_t local_ships = 0;
    for (auto it : game.me->ships) {
        if (game_map->calculate_distance(it.second->position, p) <= 3)
            ++local_ships;
    }

    // Approximate number of turns saved mining out.
    Halite halite_around = 0;
    const int CLOSE_MINE = 3;
    for (int dy = -CLOSE_MINE; dy <= CLOSE_MINE; ++dy) {
        for (int dx = -CLOSE_MINE; dx <= CLOSE_MINE; ++dx) {
            if (abs(dx) + abs(dy) > CLOSE_MINE) continue;
            Position pd(p.x + dx, p.y + dy);
            halite_around += game_map->at(pd)->halite;
        }
    }
    Halite saved =
        halite_around * ewma / MAX_HALITE *
        game_map->calculate_distance(p, game_map->at(p)->closest_base);

    // Approximate saved by returing.
    for (auto& it : game.me->ships) {
        auto ship = it.second;
        int d = game_map->calculate_distance(ship->position,
                                             game_map->at(ship)->closest_base);
        int dd = game_map->calculate_distance(ship->position, p);
        saved += max(0, d - dd) * ewma;
    }

    bool ideal = saved >= DROPOFF_COST - game_map->at(p)->halite;
    ideal &= !local_dropoffs;
    ideal &= game.turn_number <= MAX_TURNS - 50;
    ideal &= !started_hard_return;
    ideal &= local_ships >= 3;

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

    for (;;) {
        game.update_frame();
        shared_ptr<Player> me = game.me;
        unique_ptr<GameMap>& game_map = game.game_map;
        auto begin = steady_clock::now();

        vector<Command> command_queue;

        log::log("Dropoffs.");
        Halite wanted = 0;
#if 1
        for (auto it = me->ships.begin(); it != me->ships.end();) {
            auto ship = it->second;

            bool ideal = ideal_dropoff(ship->position);
            const Halite delta =
                DROPOFF_COST - game_map->at(ship)->halite - ship->halite;

            if (ideal && delta <= me->halite) {
                me->halite -= max(0, delta);
                command_queue.push_back(ship->make_dropoff());
                me->dropoffs[-ship->id] = make_shared<Dropoff>(
                    game.my_id, -ship->id, ship->position.x, ship->position.y);
                log::log("Dropoff created at", ship->position);
                new_dropoffs[ship->position] = game.turn_number;

                me->ships.erase(it++);
            } else {
                // if (ideal) wanted = wanted ? min(wanted, delta) : delta;
                ++it;
            }
        }
#endif
        while (!future_dropoffs.empty()) {
            shared_ptr<Dropoff> future_dropoff = future_dropoffs.front();
            future_dropoffs.pop();

            if (!me->dropoffs.count(future_dropoff->id)) {
                me->dropoffs[future_dropoff->id] = future_dropoff;
                new_dropoffs[future_dropoff->position] = game.turn_number;
            }
        }

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
        bool all_empty = true;
        set<Position> targets;
        for (vector<MapCell>& cell_row : game_map->cells) {
            for (MapCell& cell : cell_row) {
                Position p = cell.position;

                cell.inspired = close_enemies[p] >= INSPIRATION_SHIP_COUNT;
                cell.close_ships = 0;
                cell.closest_base = me->shipyard->position;
                for (auto& it : me->dropoffs) {
                    if (game_map->calculate_distance(p, it.second->position) <
                        game_map->calculate_distance(p, cell.closest_base)) {
                        cell.closest_base = it.second->position;
                    }
                }

                current_halite += cell.halite;
                all_empty &= !cell.halite;
                targets.insert(cell.position);
            }
        }
        halite_percentage = current_halite * 1.0 / total_halite;

        for (auto& it : me->ships)
            ++game_map->at(game_map->at(it.second)->closest_base)->close_ships;

        for (auto& player : game.players) {
            if (player->id == me->id) continue;
            for (auto& it : player->ships) {
                auto ship = it.second;
                Position p = ship->position;
                MapCell* cell = game_map->at(p);

                if (game_map->calculate_distance(p, cell->closest_base) <= 1)
                    continue;

                cell->mark_unsafe(ship);
                if (hard_stuck(ship)) continue;

                for (Position pp : p.get_surrounding_cardinals())
                    game_map->at(pp)->mark_unsafe(ship);
            }
        }

        log::log("Tasks.");
        vector<shared_ptr<Ship>> returners, explorers;
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
                    if (ship->position.doff(d) == cell->closest_base) {
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
                    if (ship->next == ship->position)
                        ship->next = cell->closest_base;
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
                    if (dist[p] == -1) {
                        uncompressed_cost.push_back(5e3);
                        continue;
                    }

                    MapCell* cell = game_map->at(p);

                    double d = game_map->calculate_distance(ship->position, p);
                    double dd = sqrt(
                        game_map->calculate_distance(p, cell->closest_base));

                    Halite profit = cell->halite - dist[p];

                    bool should_inspire =
                        game.players.size() == 4 || d <= INSPIRATION_RADIUS;
                    if (cell->inspired && should_inspire)
                        profit += INSPIRED_BONUS_MULTIPLIER * cell->halite;
                    if (d <= 3 && cell->ship &&
                        cell->ship->owner != game.my_id) {
                        profit += (1 + INSPIRED_BONUS_MULTIPLIER) *
                                  cell->ship->halite;
                    }

                    // Try to rush to highly contested areas.
                    if (game.players.size() == 4) {
                        int ed = 1e3;
                        for (auto player : game.players) {
                            if (player->id == me->id) continue;
                            for (auto it : player->ships) {
                                ed = min(ed, game_map->calculate_distance(
                                                 it.second->position, p));
                            }
                        }
                        if (d <= ed && ed - d <= 3) {
                            profit += INSPIRED_BONUS_MULTIPLIER * cell->halite;
                        }
                    }

                    // TODO: Testing rush to new dropoffs.
                    for (auto it : new_dropoffs) {
                        if (it.second + 50 < game.turn_number) continue;
                        if (game_map->calculate_distance(it.first,
                                                         cell->position) <= 3) {
                            profit += INSPIRED_BONUS_MULTIPLIER * cell->halite;
                            break;
                        }
                    }

                    if (!safe_to_move(ship, p)) profit = 0;
                    double rate = profit / max(1.0, d + dd);

                    uncompressed_cost.push_back(-rate + 5e3);
                    if (rate > 0) pq.push(uncompressed_cost.back());
                    while (pq.size() > max(explorers.size(), 25ul)) pq.pop();
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

                        if (uncompressed_cost[j] > min(top_score[i], 5e3)) {
                            cost.push_back(1e9);
                            continue;
                        }

                        auto it = targets.begin();
                        advance(it, j);

                        double best = 1.0;
                        size_t K = 15;
                        if (game_map->width <= 48 || me->ships.size() <= 50)
                            K = 25;
                        for (size_t k = 0; k < K; ++k) {
                            auto ws = random_walk(explorers[i], *it);
                            best = max(best, ws.evaluate());
                        }
                        cost.push_back(-best + 5e3);
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
            vector<map<Direction, double>> best_walks(explorers.size());
            bool timeout = false;
            while (!timeout) {
                for (size_t i = 0; i < explorers.size() && !timeout; ++i) {
                    end = steady_clock::now();
                    if (duration_cast<milliseconds>(end - begin).count() > 1500)
                        timeout = true;
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
                    if (safe_to_move(explorers[i], it.first))
                        cost_matrix[i][move_indices[it.first]] = it.second;
                    else if (game_map->at(it.first)->ship->owner != me->id)
                        cost_matrix[i][move_indices[it.first]] = 1e7;
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

#if 1
        for (auto ship : explorers) {
            Halite ideal = ideal_dropoff(ship->next);
            Halite delta = DROPOFF_COST - game_map->at(ship->next)->halite;
            if (ideal) {
                if (delta <= me->halite) {
                    log::log("Waiting on", ship->next, delta, me->halite);
                    future_dropoffs.push(make_shared<Dropoff>(
                        game.my_id, -ship->id, ship->next.x, ship->next.y));
                }
                wanted = max(wanted, delta);
            }
        }
#endif

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
        }
        should_spawn_ewma =
            game.turn_number + 2 * SHIP_COST / ewma < MAX_TURNS - 50;
        log::log("EWMA:", ewma, "Should spawn ships:", should_spawn_ewma);

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
        should_spawn &= me->ships.size() < ship_hi + 5;
        should_spawn &= halite_percentage >= 0.1;

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
