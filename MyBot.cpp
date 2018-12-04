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

// Fluorine JSON.
stringstream flog;
void message(int t, Position p, string c) {
    flog << "{\"t\": " << t << ", \"x\": " << p.x << ", \"y\": " << p.y
         << ", \"color\": \"" << c << "\"}," << endl;
}

Game game;
unordered_map<EntityId, Task> tasks;

void dijkstras(position_map<Halite>& dist, vector<Position>& sources) {
    for (vector<MapCell>& cell_row : game.game_map->cells) {
        for (MapCell& map_cell : cell_row) {
            dist[map_cell.position] = numeric_limits<Halite>::max();
        }
    }
    priority_queue<pair<Halite, Position>> pq;
    for (Position p : sources) {
        pq.emplace(0, p);
        dist[p] = 0;
    }
    while (!pq.empty()) {
        Position p = pq.top().second;
        pq.pop();
        const Halite cost = game.game_map->at(p)->halite / MOVE_COST_RATIO;
        for (Position pp : p.get_surrounding_cardinals()) {
            pp = game.game_map->normalize(pp);
            if (dist[p] + cost < dist[pp]) {
                dist[pp] = dist[p] + cost;
                pq.emplace(-dist[pp], pp);
            }
        }
    }
}

position_map<double> generate_costs(shared_ptr<Ship> ship) {
    position_map<double> surrounding_cost;

    Position p = ship->position;
    for (Position pp : p.get_surrounding_cardinals()) {
        pp = game.game_map->normalize(pp);
        surrounding_cost[pp] = 1e5;
    }

    surrounding_cost[p] = tasks[ship->id] == EXPLORE ? 1e3 : 1e7;

    for (Direction d : game.game_map->get_unsafe_moves(p, ship->next))
        surrounding_cost[game.game_map->normalize(p.directional_offset(d))] = 1;

    return surrounding_cost;
}

position_map<pair<EntityId, double>> generate_ownage() {
    position_map<pair<EntityId, double>> ownage;

    size_t my_queued = 1 + game.me->dropoffs.size();

    queue<Position> q;
    for (auto& player : game.players) {
        const pair<EntityId, double> owner(player->id, 0.0);

        ownage[player->shipyard->position] = owner;
        q.push(player->shipyard->position);
        for (auto& it : player->dropoffs) {
            ownage[it.second->position] = owner;
            q.push(it.second->position);
        }
    }

    while (!q.empty()) {
        Position p = q.front();
        auto& owner = ownage[p];
        q.pop();

        if (!my_queued) break;

        my_queued -= owner.first == game.my_id;

        for (Position pp : p.get_surrounding_cardinals()) {
            pp = game.game_map->normalize(pp);

            if (ownage.find(pp) == ownage.end()) {
                ownage[pp] = make_pair(owner.first, owner.second + 1);
                q.push(pp);

                my_queued += owner.first == game.my_id;
            } else if (owner.first != game.my_id &&
                       owner.second + 1 == ownage[pp].second) {
                ownage[pp].first = -1;
            }
        }
    }

    return ownage;
}

double evaluate_ownage(const position_map<pair<EntityId, double>>& ownage) {
    double cost = 0.0;
    for (auto& it : ownage) {
        if (it.second.first != game.my_id) continue;
        double d = max(1.0, it.second.second);
        cost += game.game_map->at(it.first)->halite / pow(d, 4);
    }
    return cost;
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

    for (;;) {
        auto begin = steady_clock::now();

        game.update_frame();
        shared_ptr<Player> me = game.me;
        unique_ptr<GameMap>& game_map = game.game_map;

        position_map<Halite> cost_to_base;
        position_map<Position> closest_base;
        position_map<bool> inspired;
        position_map<bool> is_vis;

        int want_dropoff = numeric_limits<int>::min();
        Halite halite_cutoff;

        vector<Command> command_queue;

        auto hard_stuck = [&](shared_ptr<Ship> ship) {
            const Halite left = game_map->at(ship)->halite;
            return ship->halite < left / MOVE_COST_RATIO;
        };
        auto stuck = [&](shared_ptr<Ship> ship) {
            const Halite left = game_map->at(ship)->halite;
            if (!left || ship->is_full()) return false;
            return left >= halite_cutoff;
        };

        log::log("Evaluating dropoffs.");
        auto ownage = generate_ownage();
        const double ownage_cost = evaluate_ownage(ownage);
        vector<Position> my_ownage;
        for (auto& it : ownage) {
            if (it.second.first != me->id ||
                it.second.second < game_map->width / 6) {
                continue;
            }
            my_ownage.push_back(it.first);
        }
        // TODO: It would be better to find the best dropoff every 'n' squares
        // and converge on the best.
        sort(my_ownage.begin(), my_ownage.end(),
             [&](const Position& u, const Position& v) {
                 return game_map->at(u)->halite > game_map->at(v)->halite;
             });

        vector<pair<double, Position>> new_dropoffs;
        for (size_t i = 0; i < my_ownage.size(); ++i) {
            auto t = steady_clock::now();
            if (i >= 50) break;
            if (duration_cast<milliseconds>(t - begin).count() > 150) break;

            // Attempt to build a dropoff.
            me->dropoffs[-1] = make_shared<Dropoff>(me->id, -1, my_ownage[i].x,
                                                    my_ownage[i].y);

            auto dropoff_ownage = generate_ownage();
            new_dropoffs.emplace_back(
                evaluate_ownage(dropoff_ownage), my_ownage[i]);

            me->dropoffs.erase(-1);
        }
        sort(new_dropoffs.begin(), new_dropoffs.end(),
             greater<pair<double, Position>>());
        // TODO: Do something with the dropoffs.
        log::log("Ownage", ownage_cost);
        for (size_t i = 0; i < min(3ul, new_dropoffs.size()); ++i) {
            double relative_ownage = (new_dropoffs[i].first + ownage_cost) / ownage_cost;
            if (relative_ownage >= 20) {
              log::log("Potential Ownage:", new_dropoffs[i].first);
              log::log("Relative Ownage:", relative_ownage);
              message(game.turn_number, new_dropoffs[i].second, "green");
            }
        }

        log::log("Inspiration. Closest base.");
        for (vector<MapCell>& cell_row : game_map->cells) {
            for (MapCell& map_cell : cell_row) {
                Position p = map_cell.position;

                int close_enemies = 0;
                for (auto& player : game.players) {
                    if (player->id == game.my_id) continue;
                    for (auto& it : player->ships) {
                        Position pp = it.second->position;
                        close_enemies += game_map->calculate_distance(p, pp) <=
                                         INSPIRATION_RADIUS;
                    }
                }
                inspired[p] = close_enemies >= INSPIRATION_SHIP_COUNT;

                closest_base[p] = me->shipyard->position;
                for (auto& it : me->dropoffs) {
                    if (game_map->calculate_distance(p, it.second->position) <
                        game_map->calculate_distance(p, closest_base[p])) {
                        closest_base[p] = it.second->position;
                    }
                }
            }
        }

        // Approximate cost to base..
        vector<Position> sources = {me->shipyard->position};
        for (auto& it : me->dropoffs) sources.push_back(it.second->position);
        dijkstras(cost_to_base, sources);

        // Halite distribution.
        vector<Halite> flat_halite;
        for (vector<MapCell>& cell_row : game_map->cells) {
            for (MapCell& map_cell : cell_row) {
                flat_halite.push_back(map_cell.halite);
            }
        }
        sort(flat_halite.begin(), flat_halite.end());
        halite_cutoff = flat_halite[flat_halite.size() / 2];

        // Possible targets.
        set<Position> targets;
        for (vector<MapCell>& cell_row : game_map->cells) {
            for (MapCell& map_cell : cell_row) {
                if (closest_base[map_cell.position] != map_cell.position)
                    targets.insert(map_cell.position);
            }
        }
        for (auto& player : game.players) {
            if (player->id == me->id) continue;
            for (auto& it : player->ships) {
                Position p = it.second->position;
                if (!game_map->calculate_distance(p, closest_base[p])) continue;
                if (game.players.size() == 4) {
                    targets.erase(p);
                    is_vis[p] = true;
                    for (Position pp :
                         it.second->position.get_surrounding_cardinals()) {
                        targets.erase(game_map->normalize(pp));
                        is_vis[game_map->normalize(pp)] = true;
                    }
                }
            }
        }

        log::log("Tasks.");
        vector<shared_ptr<Ship>> returners, explorers;

        double return_cutoff = 0.95;

        for (auto& it : me->ships) {
            shared_ptr<Ship> ship = it.second;
            const EntityId id = ship->id;

            int closest_base_dist = game_map->calculate_distance(
                ship->position, closest_base[ship->position]);

            // New ship.
            if (!tasks.count(id)) tasks[id] = EXPLORE;

            int return_estimate =
                game.turn_number + closest_base_dist + me->ships.size() * 0.3;
            // TODO: Dry run of return.
            if (!halite_cutoff || return_estimate >= MAX_TURNS) {
                tasks[id] = HARD_RETURN;
                started_hard_return = true;
            }

            switch (tasks[id]) {
                case EXPLORE:
                    if (ship->halite > MAX_HALITE * return_cutoff)
                        tasks[id] = RETURN;
                    break;
                case RETURN:
                    if (!closest_base_dist) tasks[id] = EXPLORE;
                case HARD_RETURN:
                    break;
            }

            // TODO: Fix stuck.
            if (hard_stuck(ship) || (stuck(ship) && tasks[id] != HARD_RETURN)) {
                command_queue.push_back(ship->stay_still());
                targets.erase(ship->position);
                is_vis[ship->position] = true;
                continue;
            }

            // Hard return.
            if (tasks[id] == HARD_RETURN && closest_base_dist <= 1) {
                for (Direction d : ALL_CARDINALS) {
                    if (ship->position.directional_offset(d) ==
                        closest_base[ship->position]) {
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
                    if (ship->position == closest_base[ship->position]) break;
                case RETURN:
                    ship->next = closest_base[ship->position];
                    returners.push_back(ship);
            }
        }

        log::log("Explorer cost matrix.");
        if (!explorers.empty()) {
            vector<vector<double>> cost_matrix;
            for (auto& ship : explorers) {
                position_map<Halite> dist;
                vector<Position> source = {ship->position};
                dijkstras(dist, source);

                vector<double> cost;
                for (Position p : targets) {
                    MapCell* map_cell = game_map->at(p);

                    double d = game_map->calculate_distance(ship->position, p);
                    double dd =
                        sqrt(game_map->calculate_distance(p, closest_base[p]));

                    Halite profit = map_cell->halite + dist[p];
                    if (inspired[p])
                        profit += INSPIRED_BONUS_MULTIPLIER * map_cell->halite;
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
                log::log("ID:", explorers[i]->id,
                         "LIVES:", explorers[i]->position,
                         "GOAL:", explorers[i]->next);
            }
        }

        // TODO: Random walk to determine edge weights.
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

            for (auto it = local_targets.begin(); it != local_targets.end();) {
                if (is_vis[*it])
                    local_targets.erase(it++);
                else
                    ++it;
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
                vector<double> cost(move_space.size(),
                                    numeric_limits<double>::max());

                position_map<double> surrounding_cost = generate_costs(ship);
                for (auto& it : surrounding_cost) {
                    if (!is_vis[it.first])
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
                    is_vis[explorers[i]->position] = true;
                    command_queue.push_back(explorers[i]->stay_still());
                }
                for (Direction d : ALL_CARDINALS) {
                    Position pp = game_map->normalize(
                        explorers[i]->position.directional_offset(d));
                    if (pp == move_space[assignment[i]]) {
                        command_queue.push_back(explorers[i]->move(d));
                        is_vis[pp] = true;
                        break;
                    }
                }
            }
        }

        log::log("Spawn ships.");
        size_t ship_lo = 0, ship_hi = numeric_limits<short>::max();
        // TODO: Smarter counter of mid-game aggression.
        if (game.turn_number <= MAX_TURNS * 0.75) {
            for (auto& player : game.players) {
                if (player->id == game.my_id) continue;
                ship_hi = min(ship_hi, player->ships.size());
                ship_lo += player->ships.size();
            }
            ship_lo /= (game.players.size() - 1);
        }

        if (me->halite >= SHIP_COST && !is_vis[me->shipyard->position] &&
            want_dropoff <= game.turn_number - 5 && !started_hard_return &&
            me->ships.size() <= ship_hi + 5 &&
            (game.turn_number <= MAX_TURNS * spawn_factor ||
             me->ships.size() < ship_lo)) {
            command_queue.push_back(me->shipyard->spawn());
        }

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
