#include "game.hpp"
#include "input.hpp"

#include <sstream>
#include <queue>
#include <map>

namespace hlt {

using namespace constants;

Game::Game() : turn_number(0) {
    std::ios_base::sync_with_stdio(false);

    populate_constants(hlt::get_string());

    int num_players;
    std::stringstream input(get_string());
    input >> num_players >> my_id;

    log::open(my_id);

    for (int i = 0; i < num_players; ++i) {
        players.push_back(Player::_generate());
    }
    me = players[my_id];
    game_map = GameMap::_generate();
}

void Game::ready(const std::string& name) {
    std::cout << name << std::endl;
}

void Game::update_frame() {
    get_sstream() >> turn_number;
    log::log("=============== TURN " + std::to_string(turn_number) + " ================");

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

    std::memset(collision, -1, sizeof collision);
    for (std::shared_ptr<Player> player : players) {
        if (player == me) continue;

        for (auto& it: player->ships) {
            Position p = it.second->position;
            collision[1][p.x][p.y] = player->id;
        }
    }
}

bool Game::end_turn(const std::vector<hlt::Command>& commands) {
    for (const auto& command : commands) {
        std::cout << command << ' ';
    }
    std::cout << std::endl;
    return std::cout.good();
}

bool Game::should_return(std::shared_ptr<Ship> ship) const {
    int return_trip = game_map->calculate_distance(ship->position, me->shipyard->position);
    for (auto& it : me->dropoffs)
        return_trip = std::min(return_trip, game_map->calculate_distance(ship->position, it.second->position));
    return return_trip + me->ships.size() + turn_number > MAX_TURNS;
}

int Game::compute_move(std::shared_ptr<Ship> ship) {
    ship->path.clear();
    switch (ship->task) {
        case kExploring:
            return compute_explore(ship);
        case kReturning:
            return compute_return(ship);
        case kNone:
            assert(false);
    }
}

int Game::compute_explore(std::shared_ptr<Ship> ship) {
    // Order positions by distance to ship.
    std::vector<Position> positions;
    positions.reserve(game_map->width * game_map->height);
    for (std::vector<MapCell> &cells : game_map->cells)
    for (MapCell map_cell : cells)
        positions.push_back(map_cell.position);
    std::sort(positions.begin(), positions.end(), [this, &ship](Position u, Position v) {
        return game_map->calculate_distance(ship->position, u) < game_map->calculate_distance(ship->position, v);
    });

    // We want an ordered map in order to cull states.
    std::map<short, char> dp[65][65];
    dp[ship->position.x][ship->position.y][ship->halite] = 0;

    std::unordered_map<short, std::pair<char, std::pair<Direction, short>>> last[65][65];

    for (Position p : positions) {
        int ship_distance = game_map->calculate_distance(p, ship->position);
        if (ship_distance >= game_map->width / 4)
            continue;

        char min_moves = -1;
        for (auto it = dp[p.x][p.y].rbegin(); it != dp[p.x][p.y].rend(); ++it) {
            Halite map_halite = game_map->at(p)->halite;
            Halite stored_halite = it->first;

            // If we can get to the same position with more halite and less moves,
            // there is no reason to process this state.
            if (min_moves != -1 && min_moves <= it->second) continue;
            min_moves = it->second;

            // Wait 't' turns and then move to a location.
            for (int t = 1; t <= 12; ++t) {
                Halite move_cost = map_halite / MOVE_COST_RATIO;

                if (move_cost <= stored_halite) {
                    for (Direction d : ALL_CARDINALS) {
                        Position pp = game_map->normalize(p.directional_offset(d));
                        if (game_map->calculate_distance(pp, ship->position) < ship_distance)
                            continue;

                        if (!dp[pp.x][pp.y].count(stored_halite - move_cost))
                            dp[pp.x][pp.y][stored_halite - move_cost] = -1;

                        char& ndp = dp[pp.x][pp.y][stored_halite - move_cost];

                        if ((ndp == -1 || it->second + t < ndp) && collision[pp.x][pp.y][t] == -1) {
                            ndp = it->second + t;
                            last[pp.x][pp.y][stored_halite - move_cost] =
                                std::make_pair(t - 1, std::make_pair(d, it->first));
                        }
                    }
                }

                Halite halite_delta = std::min(constants::MAX_HALITE - stored_halite,
                                               (map_halite + EXTRACT_RATIO - 1) / EXTRACT_RATIO);
                if (halite_delta == 0) break;
                stored_halite += halite_delta;
                map_halite -= halite_delta;
            }
        }
    }

    int score = -1;
    Position position(-1, -1);
    Halite halite = -1;

    for (std::vector<MapCell> &cells : game_map->cells) {
        for (MapCell map_cell : cells) {
            Position p = map_cell.position;
            for (auto& it : dp[p.x][p.y]) {
                if (it.first >= constants::MAX_HALITE * 19 / 20 &&
                    (score == -1 || it.second < score)) {
                    score = it.second;
                    position = p;
                    halite = it.first;
                }
            }
        }
    }

    // Backtrack path.
    while (position != ship->position) {
        auto& last_move = last[position.x][position.y][halite];
        Direction direction = last_move.second.first;
        halite = last_move.second.second;
        position = position.directional_offset(invert_direction(last_move.second.first));

        ship->path.push_back(direction);
        for (int t = 0; t < last_move.first; ++t)
            ship->path.push_back(Direction::STILL);
    }
    std::reverse(ship->path.begin(), ship->path.end());

    return ship->path.size();
}

int Game::compute_return(std::shared_ptr<Ship> ship) {
    ship->path.push_back(Direction::STILL);
    return -1;
}

Command Game::make_move(std::shared_ptr<Ship> ship) {
    if (ship->path.empty())
        compute_move(ship);

    Position p = ship->position;
    for (uint32_t i = 0; i < ship->path.size(); ++i) {
        p = game_map->normalize(p.directional_offset(ship->path[i]));
        collision[1 + i][p.x][p.y] = my_id;
    }
    return ship->move(ship->path.front());
}

bool Game::should_spawn() {
    if (me->ships.size() == 0)
        return true;
    return false;
}

}
