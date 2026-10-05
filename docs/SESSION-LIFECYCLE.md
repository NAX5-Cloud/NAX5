# Session lifecycle in the launcher

How one press of «Играть» turns into a stream and how it ends. The server's state is the truth; the launcher
follows it. Code: `gui/src/nax5/session/` (`nax5sessioncontroller.cpp` orchestrates, `nax5sessionstate.cpp` and
`nax5sessionlifecycle.cpp` hold the pure decisions, covered by `nax5sessionparser_test.cpp`).

## States

| Launcher state | Meaning | Server session |
| --- | --- | --- |
| Idle | nothing reserved | none |
| Reserving | `POST /api/v1/sessions/reserve/` in flight | none or RESERVED |
| FetchingConnection | asking for connection material | RESERVED, then CONNECTING |
| Connecting | wake-up sent, chiaki session starting | CONNECTING |
| Active | first video frame decoded | ACTIVE |
| Ending / Cancelling | telling the server the session is over | terminal |
| Error | the last attempt failed; «Играть» is available again | none |

## The happy path

1. **Reserve** with an idempotency key. The server answers with a session and a console, or refuses (see
   [MESSAGES.md](MESSAGES.md)).
2. **Connection material** for that session only (`/connection/`). It is kept in memory and wiped when the
   session ends.
3. **Wake-up.** The launcher sends the Remote Play wake-up packet to the console. This brings a console back
   from rest mode. It cannot switch on a console that is powered off.
4. **Stream.** The existing chiaki-ng session is created from the material.
5. **First decoded frame** → `/connected/`. From this moment the server counts play time.
6. **Heartbeat** every 20 seconds while the stream runs (retry after 5 seconds on a network error). The server
   releases the console when heartbeats stop for 60 seconds. With play time charged the answer carries
   `remainingSeconds`.
7. **End.** When the stream quits, the launcher posts `/end/` (or `/fail/` or `/cancel/` when it never
   connected) and repeats it until the server answers.

## How a session ends without the player

| Cause | What the launcher does | What the player sees |
| --- | --- | --- |
| Heartbeat answered 404 | stops the stream at once, posts nothing | the reason from the server, see below |
| No decoded frame for 60 s | stops the stream, posts `/end/` | the stream closes |
| The stream quits on its own | posts `/end/` or `/fail/` | chiaki's quit text |
| The launcher crashes | nothing; the server expires the session 60 s after the last heartbeat | the launcher is gone |
| Network lost | heartbeats fail; the server expires the session after 60 s | frozen picture, then the stream closes |

A 404 on a heartbeat can carry `reason`:

- `BALANCE_EXHAUSTED`: play time ran out. The panel then highlights «Пополнить».
- `SESSION_LIMIT_REACHED`: the session passed the shared-console limit while another player was refused.
- no reason: closed by an admin or already expired.

The launcher never puts the shared console to sleep. `GoToBed()` is reachable in operator mode only.

## What the launcher cannot know

- Whether the console is powered off, in rest mode or unreachable over the network: all three look like a
  connection that does not come up.
- What the player does on the console itself, including «Turn off PS5» in the console's own menu.

Both are visible only from the console's side of the network. An agent there reports the console's power
state to the backend, which then refuses to hand out a switched-off console (`CONSOLE_OFFLINE`). When the
console ends a running stream itself, chiaki reports quit reason 12 and the launcher says so.

## Pressing «Играть» again

- While a session is still active on the server, a new press ends the old session first; the server records the
  old one as `FAILED / CONNECTION_FAILED` even though it was streaming. In the admin that row is not a
  connection error.
- After «no console available» the server refuses the same player for 60 seconds with 429. Up to build 15
  the launcher blocked the button for 5 seconds only, so a player who kept pressing saw «Слишком много
  попыток» and uploaded a `reserve-fail` report each time. Build 16 blocks the button for the server's pause
  and shows a countdown.

## Timers

| Timer | Value | Where |
| --- | --- | --- |
| Heartbeat | 20 s, retry 5 s | `kHeartbeatIntervalMs`, `kHeartbeatRetryMs` |
| Stalled stream | 60 s without a decoded frame | `nax5StreamStallTimeoutMs()` |
| Button pause after a refusal | the server's `retryAfterSeconds`, else 60 s (5 s up to build 15) | `nax5ReserveRetryPauseMs()` |
| Server leases | 120 s reserved, 180 s connecting, 60 s active | backend settings |
| Server pause after «no console» | 60 s | backend `SESSION_RESERVE_BACKOFF_SECONDS` |
