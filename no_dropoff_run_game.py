import os
import sys
from random import randint, shuffle

os.system('set -e')
os.system('cmake .')
os.system('make')

bots = ["./MyBot"] + ["./bots/MyBot_Dec5NoDropoffNoStuck"] * 3
# shuffle(bots)
# bots = ["./MyBot"] + ["./bots/MyBot_Dec1NoDropoff"]

seed = randint(0, (1 << 31) - 1) if len(sys.argv) == 1 else sys.argv[1]

os.system('./halite --replay-directory replays/ -vvv --seed {} {}'.format(seed, ' '.join(bots)))
