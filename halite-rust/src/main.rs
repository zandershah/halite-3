#[macro_use]
extern crate lazy_static;

use hlt::command::Command;
// use hlt::constants::Constants;
use hlt::direction::Direction;
use hlt::game::Game;
use hlt::game_map::GameMap;
use hlt::log::Log;
use hlt::navi::Navi;
// use hlt::map_cell::MapCell;
use hlt::position::Position;
use hlt::ship::Ship;
use hlt::ShipId;

use std::collections::BinaryHeap;
use std::collections::HashMap;
use std::cmp::{Ordering, max};
// use std::env;
// use std::time::SystemTime;
// use std::time::UNIX_EPOCH;

mod hlt;

enum Task {
    Explore,
    Return,
    HardReturn
}

fn stuck(map: &GameMap, ship: &Ship) -> bool {
    ship.halite < map.at_entity(ship).halite / 10 ||
        (!ship.is_full() && map.at_entity(ship).halite >= 100)
}

fn evaluate(game: &Game, ship: &Ship, task: Option<&Task>) -> Position {
    let map = &game.map;
    let me = &game.players[game.my_id.0];

    let mut dist: Vec<Vec<i32>> = vec![vec![1 << 30; map.width]; map.height];
    dist[ship.position.x as usize][ship.position.y as usize] = 0;

    let mut pq = BinaryHeap::new();
    pq.push((0, ship.position));
    while let Some((h, p)) = pq.pop() {
        let halite = dist[p.x as usize][p.y as usize] + (map.at_position(&p).halite as i32);

        for pp in p.get_surrounding_cardinals().iter().map(|p| map.normalize(&p)) {
            if halite < dist[pp.x as usize][pp.y as usize] {
                dist[pp.x as usize][pp.y as usize] = halite;
                pq.push((-halite, pp));
            }
        }
    }

    let mut positions = Vec::new();
    match task {
        Some(Task::Explore) => {
            for cell in map.cells.iter().flatten() {
                positions.push(cell.position);
            }
        }
        _ => {
            positions.push(me.shipyard.position);
            for dropoff_id in &me.dropoff_ids {
                let dropoff = &game.dropoffs[dropoff_id];
                positions.push(dropoff.position);
            }
        }
    }

    let cost = |ship: &Ship, p: &Position| -> f64 {
        let on_dropoff = me.dropoff_ids.iter().fold(p == &me.shipyard.position, |ret, dropoff_id| {
            ret || p == &game.dropoffs[dropoff_id].position
        });

        let turn_estimate = map.calculate_distance(&ship.position, &p);

        let halite_gain_estimate = (map.at_position(&p).halite +
            p.get_surrounding_cardinals().iter().map(|pp| map.at_position(&pp).halite / 5).fold(0, |sum, v| sum + v)) as i32;
        let halite_cost_estimate = dist[p.x as usize][p.y as usize] as i32;

        match task {
            Some(Task::Explore) => {
                if on_dropoff {
                    std::f64::MAX
                } else if halite_cost_estimate >= halite_gain_estimate ||
                    6000 <= halite_gain_estimate - halite_cost_estimate {
                    std::f64::MAX
                } else {
                    ((6000 - halite_gain_estimate + halite_cost_estimate) as f64) * (max(5, turn_estimate) as f64).sqrt()
                }
            }
            _ => -(turn_estimate as f64)
        }
    };

    let mut ret_position: Position = ship.position;
    for p in positions {
        if cost(ship, &p).partial_cmp(&cost(ship, &ret_position)).unwrap_or(Ordering::Equal) == Ordering::Less {
            ret_position = p;
        }
    }
    ret_position
}

fn main() {
    let mut game = Game::new();
    let mut navi = Navi::new(game.map.width, game.map.height);
    let mut tasks = HashMap::new();

    Game::ready("RustyZanZanBot");

    loop {
        game.update_frame();
        navi.update_frame(&game);

        let map = &game.map;
        let me = &game.players[game.my_id.0];

        /*
        let mut halite: Vec<usize> = map.cells.iter().flatten().map(
            |cell| cell.halite
        ).collect();
        halite.sort();
        let q3 = halite[halite.len() * 3 / 4];
        */

        let mut command_queue: Vec<Command> = Vec::new();
        let last_turn = game.constants.max_turns;
        let mut ships: Vec<(ShipId, Position)> = Vec::new();

        for ship_id in &me.ship_ids {
            let ship = &game.ships[ship_id];
            let cell = &game.map.at_entity(ship);

            let closest_dropoff = game.map.calculate_distance(&ship.position, &me.shipyard.position);

            if !tasks.contains_key(ship_id) {
                tasks.insert(*ship_id, Task::Explore);
            }

            match tasks.get(ship_id) {
                Some(Task::Explore) => {
                    if game.turn_number + closest_dropoff + me.ship_ids.len() / 4 > last_turn {
                        tasks.insert(*ship_id, Task::HardReturn);
                    } else if ship.halite > 950 {
                        tasks.insert(*ship_id, Task::Return);
                    }
                }
                Some(Task::Return) => {
                    if closest_dropoff == 0 {
                        tasks.insert(*ship_id, Task::Explore);
                    }
                }
                _ => {}
            }

            if stuck(map, &ship) {
                command_queue.push(ship.stay_still());
                navi.mark_unsafe(&ship.position, *ship_id);
            } else {
                ships.push((*ship_id, evaluate(&game, &ship, tasks.get(ship_id))));
            }
        }
        // ships.sort_by(|a, b| a.1.partial_cmp(&b.1).unwrap_or(Equal));

        for (ship_id, p) in &ships {
            let ship = &game.ships[ship_id];
            let d: Direction = navi.naive_navigate(ship, &p);
            command_queue.push(ship.move_ship(d));
        }

        if
            game.turn_number <= last_turn / 2 &&
            me.halite >= game.constants.ship_cost &&
            navi.is_safe(&me.shipyard.position)
        {
            command_queue.push(me.shipyard.spawn());
        }

        for command in &command_queue {
            Log::log(&command.0);
        }

        Game::end_turn(&command_queue);
    }
}
