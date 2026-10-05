# NAX5 launcher changelog

What each build changed for the player and for the people who support it. Newest first.
Release facts (tag, commit, checksums) are recorded in the workspace `RELEASES.md`.

`lib/` is identical to chiaki-ng v1.10.0 in every build. `gui/src/streamsession.cpp` was changed in builds 9
and 14 and not since; the exact differences are listed in [FORK-MAINTENANCE.md](../FORK-MAINTENANCE.md).
Builds 15 and 16 change neither.

## Build 16 (not released yet)

Play time. Everything here is shown only when the backend charges play time (`billingEnforced` in
`/api/v1/auth/me/`); against a backend that does not, the launcher behaves like build 15.

- The play panel shows the balance, and the time left while a session runs, with a «Пополнить» button that
  opens the account page in the browser.
- A notice over the stream at five minutes and at one minute of play time left.
- When the server ends a session it says why: play time ran out, or the session passed the shared-console
  limit while somebody else was waiting. Starting without play time and the cooldown after that limit have
  their own messages.
- The balance is re-read after a session and when the window becomes active again.
- No play time and the cooldown do not upload a `reserve-fail` report.

Fixes from the study of build 15 logs (365 sessions):

- After a crash inside the GPU vendor's Vulkan driver the hardware decoder is switched from `auto`/`vulkan`
  to `d3d11va` on the next start, with a line on the play panel. 34 of the 38 crashes seen on build 15 were
  two players' Intel driver.
- «Играть» stays blocked for as long as the server refuses (60 seconds, or the `retryAfterSeconds` the server
  sends) with a countdown, and «too many attempts» no longer uploads a report. Before, the button came back
  after 5 seconds and every press produced a refusal and a report.
- A console that is switched off has its own answer and text (`CONSOLE_OFFLINE`) instead of a connection
  attempt that times out.
- When the console ends the stream itself (rest mode or power off chosen on it), the panel says so and asks
  the player not to power it off.
- The UDP probe no longer spins and floods the log when its socket drops out of the bound state (up to
  27 000 warning lines in one session).
- Diagnostics: BUILD-INFO lists the display adapters with driver versions (`gpu_adapters`); the crash
  summary gets a `dump_result` line, so a summary without it means the dump call never returned.

## Build 15 (2026-09-28)

- A stream with no decoded frame for 60 seconds is stopped and the console is released (console asleep or
  off the network without a disconnect).
- A heartbeat answered with 404 (session closed by an admin or expired) stops the stream.
- One launcher per Windows user: a second launch raises the first.
- `stream_sample.input_idle_s`: seconds since the last gamepad input.
- The quit reason text is written to the log.

## Build 14 (2026-09-27)

Includes build 13, which was never released on its own.

- Fixed a crash in echo cancellation (a change in `gui/src/streamsession.cpp`, microphone buffers).
- A failed stream start is retried with a paced delay instead of every 6 ms (also `streamsession.cpp`).
- Crash minidumps: written on an unhandled exception and uploaded as a `crash` report on the next login.
- `/end/` is resent until the server answers; session retries are paced.
- Whole-session totals in the diagnostics (`diagnostics_schema=4`).
- A second UDP probe, player to the console's site, next to the existing probe to the server;
  `stream_sample` gains `vps_rtt_p95_ms`, `vps_jitter_ms` and `home_*`.

## Build 12 (2026-09-24)

- UDP probe from the player to the server, a `stream_sample` line every second, live `STREAM_HEALTH` events.

## Build 11 (2026-09-24)

- Fixed a crash under Proton, restored the full settings dialog, the previous log is attached to a report.

## Builds 9 and 10 (2026-09-21)

- Mandatory update: the server can refuse an old launcher and the launcher shows where to get the new one.
- Hardened diagnostics; fixed a crash when opening settings in product mode.
- Complete session logs are delivered to the server in parts instead of a truncated tail; see
  [DIAGNOSTICS-BUILD7.md](DIAGNOSTICS-BUILD7.md) (designed as "build 7", never tagged under that number).

## Build 6 (2026-09-18)

- Heartbeat every 20 seconds while the stream is active; ZIP diagnostics; a product version string.
- Retried a refused reserve every 5 seconds on its own. Later builds retry only when the player presses
  «Играть».

Builds 7, 8 and 13 were never tagged.

The server refuses launchers older than its `NAX5_MINIMUM_CLIENT_BUILD` with the mandatory-update message.
