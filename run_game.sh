#!/usr/bin/env bash

set -e

cmake .
make
./halite --replay-directory replays/ -vvv --seed ${1:-$RANDOM} "./MyBot" "./MyBot_75" "./MyBot_Sqrt" "./MyBot_Log"
