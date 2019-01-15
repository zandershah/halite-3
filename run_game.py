import os
import sys
from random import randint, shuffle

os.system('set -e')
os.system('cmake .')
os.system('make')

bots = ['./MyBot'] + ['./bots/Jan13'] * 1

seed = randint(0, (1 << 31) - 1) if len(sys.argv) == 1 else sys.argv[1]

os.system('./halite --replay-directory replays/ --width 32 --height 32 -vvv --no-logs --no-timeout --seed {} {}'.format(seed, ' '.join(bots)))
