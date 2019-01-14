import os
import sys
from random import randint, shuffle

os.system('set -e')
os.system('cmake .')
os.system('make')

bots = ['./MyBot'] + ['./bots/Jan13NoDropoff', './bots/Jan13NoDropoff', './MyBot']
# bots = ['./MyBot'] * 2

seed = randint(0, (1 << 31) - 1) if len(sys.argv) == 1 else sys.argv[1]

os.system('./halite --no-logs --replay-directory replays/ --width 48 --height 48 --no-timeout -vvv --seed {} {}'.format(seed, ' '.join(bots)))
