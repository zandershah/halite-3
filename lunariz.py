import requests

def main():

	user = int(input("User ID? "))

	result = requests.get(
		"https://api.2018.halite.io/v1/api/user/{}/match?order_by=desc,time_played&limit=250"
		.format(user)
	).json()

	username = result[0]["players"][str(user)]["username"]

	totalGames = 0
	totalScore = 0
	duelGames = 0
	duelScore = 0
	FFAGames = 0
	FFAScore = 0
	smallGames = 0
	smallScore = 0
	largeGames = 0
	largeScore = 0
	smallDuelGames = 0
	smallDuelScore = 0
	largeDuelGames = 0
	largeDuelScore = 0
	smallFFAGames = 0
	smallFFAScore = 0
	largeFFAGames = 0
	largeFFAScore = 0

	for game in result:

		if game["challenge_id"] != None:
			continue

		resultRank = game["players"][str(user)]["rank"] - 1

		score = (100 - 100 * resultRank) if (len(game["players"]) == 2) else (100 - (100 / 3) * resultRank)

		if len(game["players"]) == 2:

			duelGames += 1
			duelScore += score

			if game["map_width"] <= 40:
				smallGames += 1
				smallScore += score
				smallDuelGames += 1
				smallDuelScore += score
			else:
				largeGames += 1
				largeScore += score
				largeDuelGames += 1
				largeDuelScore += score

		else:

			FFAGames += 1
			FFAScore += score

			if game["map_width"] <= 40:
				smallGames += 1
				smallScore += score
				smallFFAGames += 1
				smallFFAScore += score
			else:
				largeGames += 1
				largeScore += score
				largeFFAGames += 1
				largeFFAScore += score

		totalGames += 1
		totalScore += score

	print(username)
	print()

	print("For a well-rounded bot, a score of 50 in all types of games is expected.")
	print()

	print("Out of {0} total games, your average score is {1:.2f}".format(totalGames, totalScore / totalGames))
	print()

	print("Out of {0} duel games, your average score is {1:.2f}".format(duelGames, duelScore / duelGames))
	print("Out of {0} FFA games, your average score is {1:.2f}".format(FFAGames, FFAScore / FFAGames))
	print()

	print("Out of {0} small games, your average score is {1:.2f}".format(smallGames, smallScore / smallGames))
	print("Out of {0} large games, your average score is {1:.2f}".format(largeGames, largeScore / largeGames))
	print()

	print("Out of {0} small duel games, your average score is {1:.2f}".format(smallDuelGames, smallDuelScore / smallDuelGames))
	print("Out of {0} large duel games, your average score is {1:.2f}".format(largeDuelGames, largeDuelScore / largeDuelGames))
	print("Out of {0} small FFA games, your average score is {1:.2f}".format(smallFFAGames, smallFFAScore / smallFFAGames))
	print("Out of {0} large FFA games, your average score is {1:.2f}".format(largeFFAGames, largeFFAScore / largeFFAGames))
	print()



while 1:
	main()
