rm /Users/ZanderShah/Library/Application\ Support/hlt_client3/gym.db
rm replays/*
find . -name 'MyBot*' -type f -perm +111 -exec sh -c 'python3 hlt_client/client.py gym register ${0} ./${0}' {} \;
python3 hlt_client/client.py gym evaluate -i 20 -b ./halite --output-dir replays
python3 hlt_client/client.py gym stats
python3 hlt_client/client.py gym bots
