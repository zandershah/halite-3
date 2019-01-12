import os
import sys
from random import randint, shuffle

os.system('set -e')
os.system('cmake .')
os.system('make')

bots = ['./MyBot'] + ['./T1', './T2', './Test']
bots = ['./MyBot', './Test']

seed = randint(0, (1 << 31) - 1) if len(sys.argv) == 1 else sys.argv[1]

os.system('./halite --replay-directory replays/ -vvv --no-logs --width 40 --height 40 --no-timeout --seed {} {}'.format(seed, ' '.join(bots)))
