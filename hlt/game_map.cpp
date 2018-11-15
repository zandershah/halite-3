#include "game_map.hpp"
#include "input.hpp"

using namespace std;
using namespace hlt;

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
