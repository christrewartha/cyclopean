#pragma once

// Leaderboard endpoint and shared key. The real values live in LeaderboardSecrets.h, which is git-ignored
// (the repo is public). Copy LeaderboardSecrets.example.h to LeaderboardSecrets.h and fill it in.
// Without it the game still runs: the leaderboard just reports itself as offline.
#if __has_include("LeaderboardSecrets.h")
	#include "LeaderboardSecrets.h"
#endif

#ifndef CYCLOPS_LEADERBOARD_URL
	#define CYCLOPS_LEADERBOARD_URL TEXT("")
#endif

#ifndef CYCLOPS_LEADERBOARD_KEY
	#define CYCLOPS_LEADERBOARD_KEY ""
#endif
