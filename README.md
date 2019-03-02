# Halite III

> Top 10 bot for Two Sigma's artificial intelligence challenge, Halite III.

Halite III was my first AI competition, so my post-mortem is written from the perspective of one with no experience in the field. I have divided it into aspects of my bot that I think are important or interesting rather than the exact strategy that my bot executes. There are no implementation details, but the majority of my bot is in MyBot.cpp so it should not be hard to find any segment I mention.

## State (Tasks)
The port-mortems I read for Halite II strongly discouraged having state for your bot, but I was unable to avoid it for this game and I did not regret adding state to my bot.

The main reason that I kept state is that the problem of deciding whether a turtle should gather resources or return is complicated. If you just use a fixed cutoff, then the halite burned on the return path may cause your turtle to switch tasks, which is very inefficient.

A stateless idea that sounds promising is to use both the amount of halite carried and the distance to the nearest base as factors; the intuition being that as a turtle gets closer to home, it should be more ideal to continue. It is possible to define an evaluation function such that a decrease in distance outweighs the amount lost on any given move, but it would be harder to optimize and tweak. My tests showed that there was little to no difference between the high level decisions my turtles made if they held state compared using the fancier evaluation function.

So in the end, I decided to save the tasks that every turtle was assigned in the previous turn. If the turtle was assigned RETURN, it will not try to gather resources until it has dropoffed off whatever is is carrying. Turtles were usually told to return whenever they are carrying more than the fixed cutoff, but I later changed that in order to counter the late game collisions that started to become more prominent.

## Cost Functions
I used cost functions in order to help with dropoff placement, collisions, path evaluation, target selection, and move assignment. The idea behind cost functions is that mapping the set of choices to the reals allows them to be easily compared. In general, I tried to simplify the problem or find similarities between problems that I have solved before when creating cost functions.

Examples:
- The strength of an electric field is similar to the value that a potential dropoff would have.
- Maximizing the amount of halite mined can be thought of as maximizing the rate of halite mined per turn.

## Graphs
Unlike Halite II, this game was played over a discrete board, which allows us to model certain aspects as a graph. In particular, you can think of a turn as a bipartite graph (A, B), where A represents the set of cells that turtles are currently occupying, and B represents the set of cells that the turtles can occupy next turn. Each node in A then has 5 edges to B, for every possible movement.

So if we can evaluate each move and assign that cost to it's edge, we can do a maximum weighted matching on the graph in order to obtain the set of moves for your fleet. I used the Hungarian Algorithm in order to solve this.

## Stats
To evaluate the current state of the game, it would be useful to know the answer to questions such as:
1. How many turns until all halite will be mined?
2. How many turns will it take for a turtle to complete a full mining trip?

In order to answer questions such as:
3. Should I spawn a ship?
4. Should my turtle return early?

You cannot answer 1, 2 directly, but it is possible to get an estimation for them. Sampling every turn is too short of a window and will have huge amounts of noise, so I took the average over 5 turns. I used an exponentially weighted moving average after advice from a friend with a stronger stats background than me. It did not end up working as well as I hoped, but it was a good enough estimation to answer 3. I would attribute this to my lack of stats knowledge and not the technique itself.

## Local Search
The search space for potential paths between cells is extremely large, and I was not able to prove to myself any sort of weak ordering for path evaluation, so I did not use Dijkstra's. Instead, I turned to local search into order to solve paths. This was by no means optimal, I just didn't come up with a better way of solving the problem and the time limit was long enough that I could run enough random walks to obtain strong results. A much better method would be to use something simlar to reCurs3's Gaussian blur in order to simplify the problem.
