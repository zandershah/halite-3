#include "ZanZanBot.h"

using namespace std;
using namespace hlt;

ZanZanBot::ZanZanBot(hlt::Game& g) : game(g), halite(g.game_map->height, vector<Halite>(g.game_map->height)) {}

double ZanZanBot::evaluate(shared_ptr<Ship> ship) {
    unique_ptr<GameMap>& game_map = game.game_map;

    // TODO: Using Dijkstras is dumb.
    vector<vector<Halite>> dist(game_map->height,
            vector<Halite>(game_map->width, numeric_limits<Halite>::max()));
    dist[ship->position.x][ship->position.y] = 0;

    priority_queue<pair<Halite, Position>> pq;
    pq.emplace(0, ship->position);
    while (!pq.empty()) {
        Position p = pq.top().second;
        pq.pop();
        for (Position pp : p.get_surrounding_cardinals()) {
            pp = game_map->normalize(pp);
            if (dist[p.x][p.y] + halite[p.x][p.y] < dist[pp.x][pp.y]) {
                dist[pp.x][pp.y] = dist[p.x][p.y] + halite[p.x][p.y];
                pq.emplace(-dist[pp.x][pp.y], pp);
            }
        }
    }

    vector<Position> positions;
    if (tasks[ship->id] == RETURN) {
        ship->next = game.me->shipyard->position;
        for (auto& it : game.me->dropoffs)
            positions.push_back(it.second->position);
    } else if (tasks[ship->id] == EXPLORE) {
        for (vector<MapCell> &cells : game_map->cells)
        for (MapCell& cell : cells)
            positions.push_back(cell.position);
    }

    auto cost = [&](shared_ptr<Ship> ship, Position p) {
        if (tasks[ship->id] == EXPLORE && p == game.me->shipyard->position)
            return numeric_limits<double>::max();

        Halite halite_gain_estimate = game_map->at(p)->halite;
        for (Position pp : p.get_surrounding_cardinals()) {
            halite_gain_estimate += game_map->at(pp)->halite / 5;
        }
        double turn_estimate = game_map->calculate_distance(ship->position, p);
        Halite halite_cost_estimate = dist[p.x][p.y];

#if 0
        int back_distance = game_map->calculate_distance(p, game.me->shipyard->position);
        for (auto& it : game.me->dropoffs)
            back_distance = min(back_distance, game_map->calculate_distance(p, it.second->position));
        // turn_estimate += sqrt(back_distance);
#endif

        if (halite_cost_estimate >= halite_gain_estimate)
            return numeric_limits<double>::max();

        return (6000.0 - halite_gain_estimate + halite_cost_estimate) * sqrt(max(5.0, turn_estimate));
    };

    for (Position p : positions) {
        if (cost(ship, p) < cost(ship, ship->next))
            ship->next = p;
    }
    if (tasks[ship->id] == RETURN)
        return -cost(ship, ship->next);
    return cost(ship, ship->next);
}

void ZanZanBot::run() {
    for (;;) {
        game.update_frame();
        shared_ptr<Player> me = game.me;
        unique_ptr<GameMap>& game_map = game.game_map;

        vector<Halite> flat_halite;
        flat_halite.reserve(game_map->height * game_map->width);
        for (vector<MapCell> &cells : game_map->cells) {
            for (MapCell cell : cells) {
                halite[cell.position.x][cell.position.y] = cell.halite;
                flat_halite.push_back(cell.halite);
            }
        }
        sort(flat_halite.begin(), flat_halite.end());
        Halite q3 = flat_halite[flat_halite.size() * 3 / 4];

        unordered_map<shared_ptr<Ship>, double> ships;
        vector<Command> command_queue;
        auto stuck = [&](shared_ptr<Ship> ship) {
            return ship->halite < game_map->at(ship)->halite / constants::MOVE_COST_RATIO ||
                (!ship->is_full() && game_map->at(ship)->halite >= q3);
        };

        auto execute = [&](shared_ptr<Ship> ship) {
#if 0
            log::log(ship->id, "WANTS TO GO", ship->position.x, ship->position.y, "->", ship->next.x, ship->next.y);
#endif
            Direction d = game_map->naive_navigate(ship, ship->next);
            command_queue.push_back(ship->move(d));
        };

        // Tasks.
        for (const auto& ship_iterator : me->ships) {
            shared_ptr<Ship> ship = ship_iterator.second;
            EntityId id = ship->id;

            int closest_dropoff = game_map->calculate_distance(ship->position, me->shipyard->position);
            for (auto& it : me->dropoffs)
                closest_dropoff = min(closest_dropoff,
                        game_map->calculate_distance(ship->position, it.second->position));

            if (!tasks.count(id))
                tasks[id] = EXPLORE;

            if (tasks[id] == RETURN && ship->position == me->shipyard->position)
                tasks[id] = EXPLORE;
            if (tasks[id] == EXPLORE && ship->halite > constants::MAX_HALITE * 0.95)
                tasks[id] = RETURN;
            if (tasks[id] == EXPLORE &&
                    game.turn_number + closest_dropoff + (int) me->ships.size() / 4 >= constants::MAX_TURNS)
                tasks[id] = RETURN;

            if (stuck(ship)) {
                command_queue.push_back(ship->stay_still());
                game_map->at(ship)->mark_unsafe(ship);
            } else {
                ships[ship] = 0.0;
            }
        }

        while (!ships.empty()) {
            shared_ptr<Ship> ship;

            for (auto& it : ships) {
                if (!it.second) it.second = evaluate(it.first);
                if (!ship || it.second < ships[ship])
                    ship = it.first;
            }

            execute(ship);
            ships.erase(ship);
        }

        if (game.turn_number <= constants::MAX_TURNS * 0.50 && me->halite >= constants::SHIP_COST &&
                !game_map->at(me->shipyard)->is_occupied()) {
            command_queue.push_back(me->shipyard->spawn());
        }

#if 0
        for (auto& it : me->ships) {
            log::log(it.second->id, tasks[it.second->id] == EXPLORE ? "EXPLORE" : "RETURN");
        }
        for (Command c : command_queue)
            log::log(c);
#endif

        if (!game.end_turn(command_queue)) {
            break;
        }
    }
}
