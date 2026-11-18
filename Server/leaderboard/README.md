# Cyclopean leaderboard service

A small leaderboard for the testers. It is a single Python file using only the standard library, stores scores in SQLite and runs in Docker on port 8797. The game reaches it over HTTPS through your existing Cloudflare Tunnel, so no router ports need opening.

## API

| Method | Path | Notes |
|---|---|---|
| `GET` | `/api/v1/scores?limit=10` | `{"top":[{"rank","name","score"}]}`, one entry per name (their best) |
| `POST` | `/api/v1/scores` | body `{"name","score","kills","duration","version"}`, header `X-Signature` = hex HMAC-SHA1 of the raw body with `LEADERBOARD_SECRET`. Returns `{"rank","best","top"}` |
| `GET` | `/` | HTML table, for viewing in a browser |
| `GET` | `/healthz` | `ok` |

The server rejects impossible runs (score > kills x 600, or more than 3 kills/second) and limits each IP to 30 requests a minute. The shared key only stops casual tampering, because anyone who unpacks the APK can extract it. That's fine for a handful of friends.

## Deploying

Live at **https://cyclopean.christrewartha.com**, running on `scaramanga.local` in `~/cyclopean`.

### One-time setup (already done unless noted)

- SSH key login from the dev PC: `~/.ssh/config` has `Host scaramanga.local`, user `chris`, key `~/.ssh/scaramanga`.
- Tunnel ingress in `/etc/cloudflared/config.yml` (cloudflared runs on the host):
  ```yaml
  - hostname: cyclopean.christrewartha.com
    service: http://localhost:8797
  ```
  Plus the matching DNS route (`cloudflared tunnel route dns <tunnel> cyclopean.christrewartha.com`, or add it in the dashboard).
- **Still to do:** let `chris` run Docker without sudo. Run this once on the server, then log out and back in:
  ```sh
  sudo usermod -aG docker chris
  ```

### Every deploy

From Git Bash on the dev PC:

```sh
Server/deploy.sh          # or: Server/deploy.sh --logs
```

The script does the following, and is safe to re-run:
1. Copies `Server/leaderboard` to `~/cyclopean` on the server. It never copies `.env` or `data/`.
2. On first deploy only, creates `~/cyclopean/.env` with a random `LEADERBOARD_SECRET` (mode 600).
3. Runs `docker compose up -d --build`, then waits for `/healthz`.
4. Reads the secret back over SSH and writes `Source/Cyclops/Private/LeaderboardSecrets.h` (git-ignored) with the public URL and key. The game and server can't drift apart, and the secret is never printed. Rebuild the game if the script says the file changed.
5. Checks the public URL.

You can override the defaults with `CYCLOPEAN_HOST`, `CYCLOPEAN_REMOTE_DIR` and `CYCLOPEAN_URL`.

The service is bound to `127.0.0.1:8797`, so it's only reachable through the tunnel. Scores live in `~/cyclopean/data/leaderboard.sqlite3` on the server; back that up.

To rotate the key: delete `~/cyclopean/.env` on the server and redeploy. Then rebuild and re-upload the game, because older builds' submissions will be rejected.

If `LeaderboardSecrets.h` is missing, the game builds without a leaderboard and shows "Leaderboard not set up".

## Admin

On the server (`ssh scaramanga.local`, then `cd cyclopean`):

```sh
docker compose logs -f leaderboard        # follow the log
# remove a name
docker compose exec leaderboard python -c "import sqlite3; c=sqlite3.connect('/data/leaderboard.sqlite3'); c.execute(\"DELETE FROM scores WHERE name='ABC'\"); c.commit()"
# wipe everything
docker compose down && rm data/leaderboard.sqlite3 && docker compose up -d
```
