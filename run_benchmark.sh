#!/usr/bin/env bash

set -e

cmake .
make
./halite --replay-directory replays/ -vvv --width 32 --height 32 "./MyBot" "python3 ~/Downloads/benchmark/RandomBot.py"
