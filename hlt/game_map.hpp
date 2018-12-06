#pragma once

#include "map_cell.hpp"
#include "types.hpp"

#include <bits/stdc++.h>

namespace hlt {

struct GameMap {
    int width;
    int height;
    std::vector<std::vector<MapCell>> cells;

    MapCell* at(const Position& position) {
        Position normalized = normalize(position);
        return &cells[normalized.y][normalized.x];
    }

    MapCell* at(const Entity& entity) { return at(entity.position); }

    MapCell* at(const Entity* entity) { return at(entity->position); }

    MapCell* at(const std::shared_ptr<Entity>& entity) {
        return at(entity->position);
    }

    int calculate_distance(const Position& source, const Position& target) {
        const auto& normalized_source = normalize(source);
        const auto& normalized_target = normalize(target);

        const int dx = std::abs(normalized_source.x - normalized_target.x);
        const int dy = std::abs(normalized_source.y - normalized_target.y);

        const int toroidal_dx = std::min(dx, width - dx);
        const int toroidal_dy = std::min(dy, height - dy);

        return toroidal_dx + toroidal_dy;
    }

    Position normalize(const Position& position) {
        const int x = ((position.x % width) + width) % width;
        const int y = ((position.y % height) + height) % height;
        return {x, y};
    }

    std::vector<Direction> get_moves(const Position& source,
                                     const Position& destination,
                                     Halite ship_halite, Halite map_halite) {
        const auto& normalized_source = normalize(source);
        const auto& normalized_destination = normalize(destination);

        const int dx = std::abs(normalized_source.x - normalized_destination.x);
        const int dy = std::abs(normalized_source.y - normalized_destination.y);
        const int wrapped_dx = width - dx;
        const int wrapped_dy = height - dy;

        std::vector<Direction> possible_moves;

        if (map_halite) possible_moves.push_back(Direction::STILL);
        if (ship_halite < map_halite / constants::MOVE_COST_RATIO)
            return possible_moves;

        if (normalized_source.x < normalized_destination.x) {
            possible_moves.push_back(dx > wrapped_dx ? Direction::WEST
                                                     : Direction::EAST);
        } else if (normalized_source.x > normalized_destination.x) {
            possible_moves.push_back(dx < wrapped_dx ? Direction::WEST
                                                     : Direction::EAST);
        }

        if (normalized_source.y < normalized_destination.y) {
            possible_moves.push_back(dy > wrapped_dy ? Direction::NORTH
                                                     : Direction::SOUTH);
        } else if (normalized_source.y > normalized_destination.y) {
            possible_moves.push_back(dy < wrapped_dy ? Direction::NORTH
                                                     : Direction::SOUTH);
        }

        return possible_moves;
    }

    void _update();
    static std::unique_ptr<GameMap> _generate();
};

}  // namespace hlt
