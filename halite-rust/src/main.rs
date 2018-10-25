#[macro_use]
extern crate lazy_static;

use hlt::command::Command;
use hlt::constants::Constants;
use hlt::direction::Direction;
use hlt::game::Game;
use hlt::log::Log;
use hlt::navi::Navi;
use hlt::ship::Ship;
use hlt::ShipId;

use std::collections::HashMap;
use std::cmp::Ordering::Equal;
use std::env;
use std::time::SystemTime;
use std::time::UNIX_EPOCH;

mod hlt;

enum Task {
    Explore,
    Return,
    HardReturn
}

struct ZanZanBot {
    game: Game,
    navi: Navi,
    tasks: HashMap<ShipId, Task>,
}

impl ZanZanBot {
    pub fn new() -> ZanZanBot {
        let game = Game::new();
        let navi = Navi::new(game.map.width, game.map.height);
        let tasks = HashMap::new();

        ZanZanBot { game, navi, tasks }
    }

    fn stuck(&self, ship: &Ship) -> bool {
        let map_halite = self.game.map.at_entity(ship).halite;
        let move_cost_ratio = self.game.constants.move_cost_ratio;
        ship.halite < map_halite / move_cost_ratio ||
            (!ship.is_full() && map_halite >= 100)
    }

    fn evaluate(&self, ship: &Ship) -> f64 {
        0.0
    }

    pub fn run(&mut self) {
        loop {
            self.game.update_frame();
            self.navi.update_frame(&self.game);

            let me = &self.game.players[self.game.my_id.0];

            /*
            let mut halite: Vec<usize> = map.cells.iter().flatten().map(
                |cell| cell.halite
            ).collect();
            halite.sort();
            let q3 = halite[halite.len() * 3 / 4];
            */

            let mut command_queue: Vec<Command> = Vec::new();
            let last_turn = self.game.constants.max_turns;
            let mut ships: Vec<(ShipId, f64)> = Vec::new();

            for ship_id in &me.ship_ids {
                let ship = &self.game.ships[ship_id];
                let cell = self.game.map.at_entity(ship);

                let closest_dropoff = self.game.map.calculate_distance(&ship.position, &me.shipyard.position);

                if !self.tasks.contains_key(ship_id) {
                    self.tasks.insert(*ship_id, Task::Explore);
                }

                match self.tasks.get(ship_id) {
                    Some(Task::Explore) => {
                        if self.game.turn_number + closest_dropoff > last_turn {
                            self.tasks.insert(*ship_id, Task::HardReturn);
                        } else if ship.halite > 950 {
                            self.tasks.insert(*ship_id, Task::Return);
                        }
                    }
                    Some(Task::Return) => {
                        if closest_dropoff == 0 {
                            self.tasks.insert(*ship_id, Task::Explore);
                        }
                    }
                    _ => {}
                }

                if self.stuck(ship) {
                    command_queue.push(ship.stay_still());
                    self.navi.mark_safe(&ship.position);
                } else {
                    ships.push((*ship_id, self.evaluate(ship)));
                }
            }
            ships.sort_by(|a, b| a.1.partial_cmp(&b.1).unwrap_or(Equal));

            for (ship_id, _) in &ships {
                let ship = &self.game.ships[ship_id];
                let d: Direction = self.navi.naive_navigate(ship, &ship.next_position);
                command_queue.push(ship.move_ship(d));
            }

            if
                self.game.turn_number <= last_turn / 2 &&
                me.halite >= self.game.constants.ship_cost &&
                self.navi.is_safe(&me.shipyard.position)
            {
                command_queue.push(me.shipyard.spawn());
            }


            Game::end_turn(&command_queue);
        }
    }
}

fn main() {
    Game::ready("RustyZanZanBot");
    let mut z = ZanZanBot::new();
    z.run();
}
