#pragma once

// Copy this file to LeaderboardSecrets.h (git-ignored) and fill in your values.

// Base URL of the leaderboard service, no trailing slash. Quest builds need https (e.g. your Cloudflare
// Tunnel hostname). http://<LAN IP>:8000 works for PIE on the same network.
#define CYCLOPS_LEADERBOARD_URL TEXT("https://leaderboard.example.com")

// Must match LEADERBOARD_SECRET in Server/leaderboard/.env
#define CYCLOPS_LEADERBOARD_KEY "change-me-to-a-long-random-string"
