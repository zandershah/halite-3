#!/usr/bin/env bash

set -e

cmake .
make
./halite --replay-directory replays/ -vvv --seed ${1:-$RANDOM} "./MyBot" "./bots/MyBot_Nov6Inspire" "./bots/MyBot_Nov6Inspire" "./bots/MyBot_Nov6Inspire"
