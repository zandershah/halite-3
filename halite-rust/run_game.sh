#!/usr/bin/env bash

set -e

cargo build
./halite --replay-directory replays/ -vvv --seed ${1:-$RANDOM} "RUST_BACKTRACE=1 ./target/debug/my_bot" "RUST_BACKTRACE=1 ./target/debug/my_bot"
