#pragma once

#include "hlt/game.hpp"
#include "hlt/log.hpp"
#include "hlt/constants.hpp"

#include <bits/stdc++.h>

struct ZanZanBot {
    ZanZanBot(hlt::Game& g);

    double evaluate(std::shared_ptr<hlt::Ship> ship);
    void run();

    std::unordered_map<hlt::EntityId, hlt::Task> tasks;

    hlt::Game &game;
    std::vector<std::vector<hlt::Halite>> halite;
};
