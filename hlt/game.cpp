#include "game.hpp"
#include "input.hpp"

#include <sstream>

#include <queue>
#include <map>
#include <cstring>
#include <algorithm>
#include <cassert>

using namespace std;

namespace hlt {

using namespace constants;

Game::Game() : turn_number(0) {
    ios_base::sync_with_stdio(false);

    populate_constants(hlt::get_string());

    int num_players;
    stringstream input(get_string());
    input >> num_players >> my_id;

    log::open(my_id);

    for (int i = 0; i < num_players; ++i) {
        players.push_back(Player::_generate());
    }
    me = players[my_id];
    game_map = GameMap::_generate();
}

void Game::ready(const string& name) {
    cout << name << endl;
}

void Game::update_frame() {
    get_sstream() >> turn_number;
    log::log("=============== TURN " + to_string(turn_number) + " ================");

    for (size_t i = 0; i < players.size(); ++i) {
        PlayerId current_player_id;
        int num_ships;
        int num_dropoffs;
        Halite halite;
        get_sstream() >> current_player_id >> num_ships >> num_dropoffs >> halite;

        players[current_player_id]->_update(num_ships, num_dropoffs, halite);
    }

    game_map->_update();

    for (const auto& player : players) {
        for (auto& ship_iterator : player->ships) {
            auto ship = ship_iterator.second;
            game_map->at(ship)->mark_unsafe(ship);
        }

        game_map->at(player->shipyard)->structure = player->shipyard;

        for (auto& dropoff_iterator : player->dropoffs) {
            auto dropoff = dropoff_iterator.second;
            game_map->at(dropoff)->structure = dropoff;
        }
    }
}

bool Game::end_turn(const vector<hlt::Command>& commands) {
    for (const auto& command : commands) {
        cout << command << ' ';
    }
    cout << endl;
    return cout.good();
}

bool Game::should_return(shared_ptr<Ship> ship) const {
    // TODO: Fix.
    int return_trip = game_map->calculate_distance(ship->position, me->shipyard->position);
    for (auto& it : me->dropoffs)
        return_trip = min(return_trip, game_map->calculate_distance(ship->position, it.second->position));
    return (int) (return_trip + me->ships.size() + turn_number) > MAX_TURNS;
}


int Game::closest_dropoff(Position p) const {
    int closest = game_map->calculate_distance(p, me->shipyard->position);
    for (auto& it : me->dropoffs) {
      closest = min(closest, game_map->calculate_distance(p, it.second->position));
    }
    return closest;
}

int Game::compute_move(shared_ptr<Ship> ship) {
    switch (ship->task) {
        case kExploring:
            return compute_explore(ship);
        case kReturning:
            return compute_return(ship);
        case kNone:
            assert(false);
    }
    return 0;
}

int Game::compute_explore(shared_ptr<Ship> ship) {
    MapCell* map_cell = game_map->at(ship->position);

    if (ship->halite < map_cell->halite / MOVE_COST_RATIO ||
        map_cell->halite < HALITE_CUTOFF) {
        ship->next = ship->position;
        return 0;
    }

    // TODO: Assign workers to 'chunks'. Workers try to clear out their chunk.

    std::queue<Position> q;
    q.push(ship->position);
    while (!q.empty()) {
        Position p = q.front();
        q.pop();
        int dist = game_map->calculate_distance(ship->position, p);

        if (game_map->at(p)->halite > HALITE_CUTOFF) {
            ship->next = p;
            return dist;
        }

        for (Direction d : ALL_CARDINALS) {
            Position pp = game_map->normalize(p.directional_offset(d));
            if (dist < game_map->calculate_distance(ship->position, pp)) continue;
            q.push(pp);
        }
    }

    ship->next = ship->position;
    return 0;
}

int Game::compute_return(shared_ptr<Ship> ship) {
    ship->next = me->shipyard->position;
    for (auto& it : me->dropoffs) {
        if (game_map->calculate_distance(ship->position, it.second->position) <
            game_map->calculate_distance(ship->position, ship->next))
            ship->next = it.second->position;
    }
    return game_map->calculate_distance(ship->position, ship->next);
}

Command Game::make_move(shared_ptr<Ship> ship) {
    return ship->move(game_map->naive_navigate(ship, ship->next));
}

bool Game::should_spawn() {
    if (me->ships.size() == 0)
        return true;
    return false;
}

}
