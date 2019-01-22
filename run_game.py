import os
import sys
from random import randint, shuffle

os.system('set -e')
os.system('cmake .')
os.system('make')

test_bot = './Test'

bots = ['./MyBot'] + [test_bot] * 3
bots = ['./MyBot', test_bot]

seed = randint(0, (1 << 31) - 1) if len(sys.argv) == 1 else sys.argv[1]

os.system('./halite --replay-directory replays/ -vvv --width 32 --height 32 --no-logs --no-timeout --seed {} {}'.format(seed, ' '.join(bots)))
