#include "hlt/game.hpp"
#include "ZanZanBot.h"

int main(int argc, char* argv[]) {
    hlt::Game game;
    // At this point "game" variable is populated with initial map data.
    // This is a good place to do computationally expensive start-up pre-processing.
    // As soon as you call "ready" function below, the 2 second per turn timer will start.
    game.ready("ZanZanBot");
    ZanZanBot z(game);
    z.run();
}
