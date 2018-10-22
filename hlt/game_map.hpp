#pragma once

#include "types.hpp"
#include "map_cell.hpp"

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

        MapCell* at(const Entity& entity) {
            return at(entity.position);
        }

        MapCell* at(const Entity* entity) {
            return at(entity->position);
        }

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
            return { x, y };
        }

        std::vector<Direction> get_unsafe_moves(const Position& source, const Position& destination) {
            const auto& normalized_source = normalize(source);
            const auto& normalized_destination = normalize(destination);

            const int dx = std::abs(normalized_source.x - normalized_destination.x);
            const int dy = std::abs(normalized_source.y - normalized_destination.y);
            const int wrapped_dx = width - dx;
            const int wrapped_dy = height - dy;

            std::vector<Direction> possible_moves;

            if (normalized_source.x < normalized_destination.x) {
                possible_moves.push_back(dx > wrapped_dx ? Direction::WEST : Direction::EAST);
            } else if (normalized_source.x > normalized_destination.x) {
                possible_moves.push_back(dx < wrapped_dx ? Direction::WEST : Direction::EAST);
            }

            if (normalized_source.y < normalized_destination.y) {
                possible_moves.push_back(dy > wrapped_dy ? Direction::NORTH : Direction::SOUTH);
            } else if (normalized_source.y > normalized_destination.y) {
                possible_moves.push_back(dy < wrapped_dy ? Direction::NORTH : Direction::SOUTH);
            }

            std::sort(possible_moves.begin(), possible_moves.end(), [&](Direction u, Direction v) {
                return at(source.directional_offset(u))->halite < at(source.directional_offset(v))->halite;
            });

            return possible_moves;
        }

        Direction naive_navigate(std::shared_ptr<Ship> ship, const Position& destination, Task task) {
            // get_unsafe_moves normalizes for us
            for (Direction direction : get_unsafe_moves(ship->position, destination)) {
                Position target_pos = ship->position.directional_offset(direction);
                if (!at(target_pos)->is_occupied()) {
                    if (task != HARD_RETURN || target_pos != destination)
                        at(target_pos)->mark_unsafe(ship);
                    return direction;
                }
            }

            if (!at(ship)->is_occupied()) {
                if (task != HARD_RETURN || ship->position != destination)
                    at(ship)->mark_unsafe(ship);
                return Direction::STILL;
            }

            for (Direction d : ALL_CARDINALS) {
                Position target_pos = ship->position.directional_offset(d);
                if (!at(target_pos)->is_occupied()) {
                    if (task != HARD_RETURN || target_pos != destination)
                        at(target_pos)->mark_unsafe(ship);
                    return d;
                }
            }

            return Direction::STILL;
        }

        void _update();
        static std::unique_ptr<GameMap> _generate();
    };
}
