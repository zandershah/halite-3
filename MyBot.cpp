#include "hlt/game.hpp"
#include "hlt/constants.hpp"
#include "hlt/log.hpp"

#include <bits/stdc++.h>

using namespace std;
using namespace hlt;

unsigned int rng_seed;
mt19937 rng;

void assign_tasks(Game& game) {
    for (const auto& it : game.me->ships) {
        shared_ptr<Ship> ship = it.second;

        if (ship->task == Task::kReturning)
          continue;

        // TODO: Randomize should_return so that all bots don't attempt to return at the same time.
        if (ship->halite >= constants::MAX_HALITE * 19 / 20 || game.should_return(ship)) {
            ship->task = Task::kReturning;
        } else {
            ship->task = Task::kExploring;
        }
    }
}

void evaluate_tasks(Game& game) {}

vector<Command> execute_tasks(Game& game) {
    unordered_set<shared_ptr<Ship>> ships;
    for (const auto& it : game.me->ships) {
        ships.insert(it.second);
    }
    vector<Command> command_queue;
    while (!ships.empty()) {
        auto it = max_element(ships.begin(), ships.end(), [&game](shared_ptr<Ship> u, shared_ptr<Ship> v) {
            return game.compute_move(u) < game.compute_move(v);
        });
        command_queue.push_back(game.make_move(*it));
        ships.erase(it);
    }
    if (game.should_spawn()) {
        command_queue.push_back(game.me->shipyard->spawn());
    }
    return command_queue;
}

int main(int argc, char* argv[]) {
    if (argc > 1) {
        rng_seed = static_cast<unsigned int>(stoul(argv[1]));
    } else {
        rng_seed = static_cast<unsigned int>(time(nullptr));
    }
    rng.seed(rng_seed);

    Game game;
    // At this point "game" variable is populated with initial map data.
    // This is a good place to do computationally expensive start-up pre-processing.
    // As soon as you call "ready" function below, the 2 second per turn timer will start.
    game.ready("ZanderBotCpp");

    while (1) {
        game.update_frame();
        assign_tasks(game);
        evaluate_tasks(game);
        if (!game.end_turn(execute_tasks(game))) break;
    }

    return 0;
}
