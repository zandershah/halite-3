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

bool started_hard_return = false;

double average_halite_left = 0.0;

unordered_map<EntityId, int> last_moved;

inline Halite extracted(Halite h) {
    return (h + EXTRACT_RATIO - 1) / EXTRACT_RATIO;
}

shared_ptr<Dropoff> future_dropoff;
set<Position> future_collisions;
map<Position, int> recent_collisions;

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
bool safe_to_move(shared_ptr<Ship> ship, Position p, bool print = false) {
    unique_ptr<GameMap>& game_map = game.game_map;
    MapCell* cell = game_map->at(p);

    if (!cell->is_occupied()) return true;

    if (ship->owner == cell->ship->owner) return false;
    if (cell->has_structure() && cell->structure->id != -2)
        return cell->structure->owner == game.my_id;
    if (tasks[ship->id] == HARD_RETURN) return true;

    // They shouldn't be walking over this.
    if (!cell->really_there &&
        MAX_HALITE - cell->ship->halite < extracted(cell->halite)) {
        return true;
    }
    Halite dropped = ship->halite + cell->ship->halite;
    Halite already = cell->halite;
    if (cell->inspired()) {
        dropped += INSPIRED_BONUS_MULTIPLIER * dropped;
        already += INSPIRED_BONUS_MULTIPLIER * already;
    }

    // Estimate who is closer.
    if (!safe_to_move_cache.count(p)) {
        int closeness = 0;
        for (auto player : game.players) {
            for (auto& it : player->ships) {
                if (it.second->id == cell->ship->id) continue;
                if (MAX_HALITE - it.second->halite <
                    extracted(dropped + already))
                    continue;
                if (player->id == game.my_id && tasks[it.second->id] != EXPLORE)
                    continue;
                int d = game_map->calc_dist(p, it.second->position);
                if (player->id == game.my_id)
                    closeness += d <= 3;
                else
                    closeness -= d <= 3;
            }
        }
        safe_to_move_cache[p] = closeness;
    }

    int closeness = safe_to_move_cache[p];
    if (MAX_HALITE - ship->halite < extracted(dropped + already))
        closeness -= game_map->calc_dist(p, ship->position) <= 3;

    if (print) {
        log::log(ship->id, "From:", ship->position, "To:", p,
                 "Closeness:", closeness, "Dropped:", dropped);
    }

    if (closeness <= 0 ||
        ship->halite > cell->ship->halite + MAX_HALITE * 0.25) {
        return false;
    }
    if (game.players.size() == 2) return true;
    return dropped >= min(1.5 * SHIP_COST, 3 * average_halite_left);
}

void bfs(position_map<Halite>& dist, shared_ptr<Ship> ship) {
    unique_ptr<GameMap>& game_map = game.game_map;

    dist.clear();
    position_map<bool> vis;

    queue<Position> q;
    q.push(ship->position);
    dist[ship->position] = 0;
    while (!q.empty()) {
        Position p = q.front();
        q.pop();

        if (game.players.size() == 4 && !safe_to_move(ship, p)) continue;

        const Halite cost = game_map->at(p)->halite / MOVE_COST_RATIO;
        for (Position pp : p.get_surrounding_cardinals()) {
            pp = game_map->normalize(pp);
            if (game_map->calc_dist(ship->position, pp) <=
                game_map->calc_dist(ship->position, p)) {
                continue;
            }

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
        if (game.game_map->at(p)->inspired()) {
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
        Halite h = ship_halite - burned_halite;
        if (game.game_map->at(p)->really_there)
            h += game.game_map->at(p)->ship->halite;
        double rate;
        if (tasks[ship_id] == EXPLORE) {
            rate = (h - starting_ship_halite) / turns;
        } else {
            rate = h / pow(turns, 4);
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
            return !safe_to_move(ship, ws.p.doff(d));
        });
        moves.erase(rit, moves.end());
        if (moves.empty()) {
            // TODO: Add sideways moves when only waking in a line.
            // We try to add all moves.
            for (Direction d : ALL_CARDINALS) {
                if (safe_to_move(ship, ws.p.doff(d))) moves.push_back(d);
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

    int close_dropoff = game.players.size() == 2 ? 20 : 15;

    bool local_dropoffs = game_map->at(p)->has_structure();
    local_dropoffs |=
        game_map->calc_dist(p, game.me->shipyard->position) <= close_dropoff;
    for (auto& it : game.me->dropoffs)
        local_dropoffs |=
            game_map->calc_dist(p, it.second->position) <= close_dropoff;

    int close_check = 5;
    Halite halite_around = 0;
    double s = 0;
    for (int dy = -close_check; dy <= close_check; ++dy) {
        for (int dx = -close_check; dx <= close_check; ++dx) {
            if (abs(dx) + abs(dy) > close_check) continue;
            Position pd(p.x + dx, p.y + dy);

            ++s;

            if (!ideal_dropoff_cache.count(pd)) {
                priority_queue<int> ally_pq, enemy_pq;
                for (auto player : game.players) {
                    for (auto it : player->ships) {
                        int d = game_map->calc_dist(pd, it.second->position);
                        if (player->id == game.my_id)
                            ally_pq.push(d);
                        else
                            enemy_pq.push(d);
                        while (ally_pq.size() > 3) ally_pq.pop();
                        while (enemy_pq.size() > 3) enemy_pq.pop();

                        if (player->id != game.my_id &&
                            it.second->position == p) {
                            return 0;
                        }
                    }
                }
                int ally = 0, enemy = 0;
                while (!ally_pq.empty()) {
                    ally += ally_pq.top();
                    ally_pq.pop();
                }
                while (!enemy_pq.empty()) {
                    enemy += enemy_pq.top();
                    enemy_pq.pop();
                }
                ideal_dropoff_cache[pd] = ally / 3 - enemy / 3;
            }

            if (ideal_dropoff_cache[pd] <= 2)
                halite_around += game_map->at(pd)->halite;
        }
    }

    Halite saved = halite_around;

    bool ideal = saved >= DROPOFF_COST + SHIP_COST;
    ideal &= !local_dropoffs;
    ideal &= game.turn_number <= MAX_TURNS - 50;
    ideal &= !started_hard_return;
    // ideal &= local_ships >= 3;

    double bases = 2.0 + game.me->dropoffs.size();
    int bb = 7;
    if (game.players.size() == 4 && game_map->width <= 32) bb = 5;
    ideal &= game.me->ships.size() / bases >= bb;

    return ideal * saved * sqrt(game_map->at(p)->halite);
}

int main(int argc, char* argv[]) {
    game.ready("BabuBot");

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

        int total_ships = 0;
        for (auto player : game.players) total_ships += player->ships.size();

        for (auto& it : me->ships) future_collisions.erase(it.second->position);

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
                message(future_dropoff->position, "blue");
            }
        }

        auto end = steady_clock::now();
        log::log("Millis: ", duration_cast<milliseconds>(end - begin).count());

        log::log("Inspiration. Closest base.");
        position_map<int> close_enemies, close_allies;
        for (auto& player : game.players) {
            const int IR = INSPIRATION_RADIUS;
            for (auto& it : player->ships) {
                Position p = it.second->position;
                for (int dx = -IR; dx <= IR; ++dx) {
                    for (int dy = -IR; dy <= IR; ++dy) {
                        if (abs(dx) + abs(dy) > IR) continue;
                        if (player->id == me->id)
                            ++close_allies[game_map->normalize(
                                Position(p.x + dx, p.y + dy))];
                        else
                            ++close_enemies[game_map->normalize(
                                Position(p.x + dx, p.y + dy))];
                    }
                }
            }
        }

        multiset<Position> targets;
        for (Position p : future_collisions) {
            recent_collisions[p] = game.turn_number;
            log::log("Collision at", p);
        }
        future_collisions.clear();
        for (auto it = recent_collisions.begin();
             it != recent_collisions.end();) {
            if (game.turn_number - it->second >= 5) {
                recent_collisions.erase(it++);
            } else {
                targets.insert(it->first);
                targets.insert(it->first);
                ++it;
            }
        }

        Halite current_halite = 0;
        bool all_empty = true;
        for (vector<MapCell>& cell_row : game_map->cells) {
            for (MapCell& cell : cell_row) {
                Position p = cell.position;

                cell.really_there = false;
                cell.close_enemies = close_enemies[p];
                cell.close_allies = close_allies[p];
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
                targets.insert(p);
            }
        }

        average_halite_left = current_halite * 1.0 / total_ships;

        for (auto& player : game.players) {
            if (player->id == me->id) continue;
            for (auto& it : player->ships) {
                if (it.second->halite > average_halite_left) {
                    // Fight them.
                    // targets.insert(it.second->position);
                    // targets.insert(it.second->position);
                }
            }
        }

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

                cell->mark_unsafe(ship);
                cell->really_there = true;
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

            double return_turn =
                game_map->calc_dist(ship->position, cell->closest_base);

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

        set<Position> fresh_dropoffs;
        for (auto it : me->dropoffs) {
            int close_check = 5;
            Halite halite_around = 0;
            for (int dy = -close_check; dy <= close_check; ++dy) {
                for (int dx = -close_check; dx <= close_check; ++dx) {
                    if (abs(dx) + abs(dy) > close_check) continue;
                    Position pd(it.second->position.x + dx,
                                it.second->position.y + dy);

                    halite_around += game_map->at(pd)->halite;
                }
            }
            if (halite_around >= DROPOFF_COST + SHIP_COST)
                fresh_dropoffs.insert(it.second->position);
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

            double halite_cutoff =
                max(HALITE_RETURN * 0.1, 3 * average_halite_left);
            halite_cutoff = min(halite_cutoff, HALITE_RETURN);
            switch (tasks[id]) {
                case EXPLORE:
                    if (ship->halite > halite_cutoff) tasks[id] = RETURN;
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
                future_collisions.insert(ship->position);
                game_map->at(ship)->ship = ship;
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
                    ship->next = cell->closest_base;
                    for (Position fresh : fresh_dropoffs) {
                        int d_close =
                            game_map->calc_dist(ship->position, ship->next);
                        int d_new = game_map->calc_dist(ship->position, fresh);
                        int d_apart = game_map->calc_dist(ship->next, fresh);
                        if (d_close > 5 && d_new <= d_apart &&
                            (!fresh_dropoffs.count(ship->next) ||
                             d_new < d_close)) {
                            ship->next = fresh;
                        }
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
                bfs(dist, ship);
                priority_queue<double> pq;

                vector<double> uncompressed_cost;
                uncompressed_cost.reserve(targets.size());
                for (Position p : targets) {
                    MapCell* cell = game_map->at(p);

                    double d = game_map->calc_dist(ship->position, p);
                    double dd = game_map->calc_dist(p, cell->closest_base);

                    if (!dist.count(p)) dist[p] = 1e3;
                    Halite profit = cell->halite - dist[p];

                    const int IBS = INSPIRED_BONUS_MULTIPLIER;

                    bool future_inspire =
                        future_dropoff &&
                        game_map->calc_dist(future_dropoff->position, p) <= 3 &&
                        (!fresh_dropoffs.count(
                             game_map->at(ship)->closest_base) ||
                         game_map->at(ship)->closest_base ==
                             future_dropoff->position);

                    if (cell->inspired() || future_inspire)
                        profit += IBS * cell->halite;

                    if (cell->ship && cell->ship->owner != game.my_id &&
                        cell->really_there &&
                        (game.players.size() == 2 ||
                         cell->halite > average_halite_left)) {
                        Halite collision_halite = cell->ship->halite;
                        if (cell->inspired() || future_inspire)
                            collision_halite += IBS * collision_halite;
                        if (profit + ship->halite < collision_halite)
                            profit += collision_halite;
                    }

                    profit = min(profit, MAX_HALITE - ship->halite);

                    if (!safe_to_move(ship, p)) profit = 0;
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
            int timeout_walks = 0;
            end = steady_clock::now();
            while (!timeout) {
                for (size_t i = 0; i < explorers.size() && !timeout; ++i) {
                    if (duration_cast<milliseconds>(steady_clock::now() - end)
                            .count() > 750) {
                        log::log("Was able to do", timeout_walks,
                                 "random walks.");
                        timeout = true;
                    }
                    if (duration_cast<milliseconds>(steady_clock::now() - begin)
                            .count() > 1750) {
                        timeout = true;
                    }
                    ++timeout_walks;

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

                bool print = false;
                for (auto& it : surrounding_cost) {
                    if (safe_to_move(explorers[i], it.first)) {
                        cost_matrix[i][move_indices[it.first]] = it.second;
                    } else if (game_map->at(it.first)->ship->owner != me->id) {
                        safe_to_move(explorers[i], it.first, true);
                        print = true;

                        // Four cases.
                        Halite enemy_halite =
                            game_map->at(it.first)->ship->halite;
                        if (game_map->at(it.first)->really_there) {
                            if (explorers[i]->halite <
                                enemy_halite - MAX_HALITE * 0.25) {
                                // We have at least 250 less halite. They don't
                                // want to collide.
                                cost_matrix[i][move_indices[it.first]] = 1e4;
                            } else {
                                cost_matrix[i][move_indices[it.first]] = 1e7;
                            }
                        } else {
                            if (enemy_halite <
                                explorers[i]->halite + MAX_HALITE * 0.25) {
                                cost_matrix[i][move_indices[it.first]] = 1e7;
                            } else {
                                cost_matrix[i][move_indices[it.first]] = 1e6;
                            }
                        }
                    }
                }
                if (print) {
                    log::log("Ship", explorers[i]->id);
                    for (auto& it : surrounding_cost) {
                        safe_to_move(explorers[i], it.first, true);
                        log::log(it.first, it.second);
                    }
                    log::log("Done.");
                }
            }

            // Solve and execute moves.
            vector<int> assignment(explorers.size());
            HungarianAlgorithm ha;
            ha.Solve(cost_matrix, assignment);

            for (size_t i = 0; i < assignment.size(); ++i) {
                if (explorers[i]->position == move_space[assignment[i]]) {
                    game_map->at(explorers[i])->ship = explorers[i];
                    command_queue.push_back(explorers[i]->stay_still());
                    future_collisions.insert(explorers[i]->position);
                }
                for (Direction d : ALL_CARDINALS) {
                    Position pp =
                        game_map->normalize(explorers[i]->position.doff(d));
                    if (pp == move_space[assignment[i]]) {
                        command_queue.push_back(explorers[i]->move(d));
                        future_collisions.insert(pp);
                        game_map->at(pp)->ship = explorers[i];
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
                d += game_map->calc_dist(p, it.second->position);
            d /= me->ships.size();

            futures.emplace_back(p, ideal / d);
        }
        sort(futures.begin(), futures.end(),
             [&](pair<Position, double> u, pair<Position, double> v) {
                 return u.second > v.second;
             });
        if (!futures.empty() && !future_dropoff) {
            wanted = DROPOFF_COST - game_map->at(futures.front().first)->halite;

            Halite fluff = 0;
            // Turns before.
            for (auto ship : explorers) {
                if (tasks[ship->id] != RETURN ||
                    game_map->calc_dist(ship->position, ship->next) > 5) {
                    continue;
                }
                fluff += ship->halite * 0.95;
            }

            if (wanted - fluff <= me->halite) {
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
            should_spawn_ewma =
                game.turn_number + 2 * SHIP_COST / ewma < MAX_TURNS;

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

        bool should_spawn = !game_map->at(me->shipyard)->is_occupied();
        should_spawn &= !started_hard_return;
        should_spawn &= 3 * average_halite_left > SHIP_COST;
        should_spawn &= should_spawn_ewma || me->ships.size() < ship_lo;
        should_spawn &= me->ships.size() < ship_hi + 5;

        // Expected return in the next few turns, in order to do wanted better.
        Halite fluff = 0;
        if (future_dropoff) {
            // Turns before.
            int d = 1e3;
            for (auto ship : explorers) {
                d = min(d, game_map->calc_dist(ship->position,
                                               future_dropoff->position));
                if (tasks[ship->id] != RETURN ||
                    ship->next == future_dropoff->position)
                    continue;
                if (game_map->calc_dist(ship->position, ship->next) < d)
                    fluff += ship->halite * 0.95;
            }

            if (fluff) log::log("Fluff!", fluff);
        }
        should_spawn &= me->halite >= SHIP_COST + max(0, wanted - fluff);

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
