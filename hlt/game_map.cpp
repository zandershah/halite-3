#include "game_map.hpp"
#include "input.hpp"

using namespace std;
using namespace hlt;

Direction GameMap::navigate_return(shared_ptr<Ship> ship, Task task) {
    struct State {
        State() : t(numeric_limits<int>::max()), p(0, 0) {}
        State(size_t tt, Halite hh, Position pp) : t(tt), h(hh), p(pp) {}
        size_t t;
        Halite h;
        Position p;

        bool operator<(State& s) const {
            if (t == s.t)
                return h > s.h;
            return t < s.t;
        }
    };

    struct StateCompare {
        // Reversed for priority_queue.
        bool operator()(State& u, State& v) const {
            return v < u;
        }
    };

    // Position represents last.
    vector<vector<State>> dp(height, vector<State>(width));
    // Position represents current.
    priority_queue<State, vector<State>, StateCompare> pq;
    pq.emplace(0, ship->halite, normalize(ship->position));
    dp[pq.top().p.x][pq.top().p.y] = pq.top();
    while (!pq.empty()) {
        State s = pq.top();
        pq.pop();

        if (s.p == ship->next)
            break;
        if (dp[s.p.x][s.p.y] < s)
            continue;

        // Try to wait 'n' turns.
        Halite ship_halite = s.h;
        Halite left_halite = at(s.p)->halite;
        for (size_t i = 0; i <= 5; ++i) {
            const Halite move_cost = left_halite / constants::MOVE_COST_RATIO;

            if (is_vis(s.p, s.t + i))
                break;
            if (ship_halite < move_cost)
                continue;

            State ss(s.t + i + 1, ship_halite - move_cost, s.p);
            for (Position p : s.p.get_surrounding_cardinals()) {
                p = normalize(p);
                if (ss < dp[p.x][p.y] && !is_vis(p, ss.t)) {
                    dp[p.x][p.y] = ss;
                    pq.emplace(ss.t, ss.h, p);
                }
            }

            Halite delta_halite = (left_halite  + constants::EXTRACT_RATIO - 1) / constants::EXTRACT_RATIO;
            delta_halite = min(delta_halite, constants::MAX_HALITE - ship_halite);

            ship_halite += delta_halite;
            left_halite -= delta_halite;
        }
    }

    vector<Position> path;
    path.push_back(normalize(ship->next));
    while (path.back() != ship->position) {
        State& s = dp[path.back().x][path.back().y];
        if (s.t == numeric_limits<int>::max())
            break;
        for (size_t t = s.t; t > dp[s.p.x][s.p.y].t; --t)
            path.push_back(s.p);
    }

    if (path.back() != ship->position || path.size() == 1) {
        if (task != HARD_RETURN)
            mark_vis(ship->position, 1);
        return Direction::STILL;
    }

    reverse(path.begin(), path.end());
    for (size_t i = 0; i < path.size(); ++i) {
        if (task != HARD_RETURN || i + 1 != path.size())
            mark_vis(path[i], i);
    }

    for (Direction d : ALL_CARDINALS) {
        if (normalize(path[0].directional_offset(d)) == path[1])
            return d;
    }
    return Direction::STILL;
}

void GameMap::_update() {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            cells[y][x].ship.reset();
        }
    }

    int update_count;
    get_sstream() >> update_count;

    for (int i = 0; i < update_count; ++i) {
        int x;
        int y;
        int halite;
        get_sstream() >> x >> y >> halite;
        cells[y][x].halite = halite;
    }

    vis.clear();
}

unique_ptr<GameMap> GameMap::_generate() {
    unique_ptr<GameMap> map = make_unique<GameMap>();

    get_sstream() >> map->width >> map->height;

    map->cells.resize((size_t)map->height);
    for (int y = 0; y < map->height; ++y) {
        auto in = get_sstream();

        map->cells[y].reserve((size_t)map->width);
        for (int x = 0; x < map->width; ++x) {
            Halite halite;
            in >> halite;

            map->cells[y].push_back(MapCell(x, y, halite));
        }
    }

    return map;
}
