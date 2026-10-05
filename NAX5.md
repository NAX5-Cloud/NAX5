# NAX5 Client

Native Windows client for NAX5. This repository is a **minimal fork** of [streetpea/chiaki-ng](https://github.com/streetpea/chiaki-ng) at **v1.10.0**.

Root [README.md](README.md) is the product/user-facing note. This file is the developer guide. Upstream licenses and notices stay in [UPSTREAM.md](UPSTREAM.md), [FORK-MAINTENANCE.md](FORK-MAINTENANCE.md), [COPYING](COPYING), and [LICENSES](LICENSES).

Releases are cut from tags `alpha-0.5-build-N`; the source of a release is its tag. The default branch
`task4-auto-remote-play` lags behind (build 12): builds 14 to 16 live in stacked branches that are not merged
yet. Record tagged releases in workspace [RELEASES.md](../RELEASES.md).

## Read next

| Document | What it answers |
| --- | --- |
| [docs/CHANGELOG.md](docs/CHANGELOG.md) | what each build changed |
| [docs/SESSION-LIFECYCLE.md](docs/SESSION-LIFECYCLE.md) | states, heartbeat, how a session ends, timers |
| [docs/MESSAGES.md](docs/MESSAGES.md) | on-screen texts and the error numbers in logs |
| [docs/SUPPORT.md](docs/SUPPORT.md) | typical player complaints and what to check |
| [KNOWN-ISSUES.md](KNOWN-ISSUES.md) | open problems |
| [docs/DIAGNOSTICS-BUILD7.md](docs/DIAGNOSTICS-BUILD7.md) | how logs reach the server |

## Purpose

Give an authenticated NAX5 user a Play button that:

1. talks to NAX5 Backend for login, Console assignment, and Session lifecycle;
2. receives **transient** Remote Play connection material;
3. starts the **existing** chiaki-ng stream session against a physical PS5.

NAX5 uses chiaki-ng because Remote Play already works there. The product does not invent a second streaming stack.

## Owns

- NAX5 identity, settings isolation, Windows packaging
- Native control-plane HTTP client (`X-Session-Token`)
- Session reservation / connection / connected / fail / end
- Transient `StreamSessionConnectInfo` adapter
- Product and operator QML chrome (`LoginView`, `Nax5PlayPanel`, `Nax5AccountBar`, `Nax5OperatorPanel`)
- Process logging (`nax5processlog`) and client-reports upload (`nax5clientreport`)

## Does not own

- Remote Play protocol, decode/render, audio, input, pacing, codecs, crypto, streaming sockets
- Django API behavior or Caddy routing
- Website UI
- Durable PS5 secret storage (backend runtime data)

## Main entry points

- `gui/src/main.cpp` — Qt application; QSettings org/app come from `Nax5Runtime`
- `gui/src/qml/Main.qml` + `gui/src/qml/LoginView.qml` — login gate, then product UI
- `gui/src/nax5/nax5authcontroller.cpp` — native login / me / logout
- `gui/src/nax5/session/nax5sessioncontroller.cpp` — Play orchestration
- `gui/src/nax5/connection/nax5transienthost.cpp` — fills chiaki `StreamSessionConnectInfo`
- `gui/src/qmlbackend.cpp` — existing stream session owner (`createSession`, disconnect)
- `gui/src/nax5/nax5processlog.cpp` — `%AppData%/NAX5/NAX5/log/nax5_*.log`
- `gui/src/nax5/nax5clientreport.cpp` — ZIP export + `POST /api/v1/client-reports/`

Default API base is `https://cloudgta6.com`. Override locally with `NAX5_API_BASE_URL` (loopback `http` allowed).

Native login uses `/_allauth/app/v1/auth/login`. Session APIs live under `/api/v1/sessions/` on the **apex** host only (`cloudgta6.com`, not `www`). See `nax5-backend/docs/NATIVE-EDGE-DESIGN.md`.

## Product code location

Put new product logic in:

- `gui/include/nax5/`
- `gui/src/nax5/`
  - `nax5/` — auth, API client, runtime, logging, reports
  - `nax5/session/` — assignment and lifecycle
  - `nax5/connection/` — material parse + transient host

Product QML currently lives next to upstream QML:

- `gui/src/qml/Nax5*.qml`
- `gui/src/qml/LoginView.qml`

Keep QML product-named. Avoid new Remote Play behavior in `lib/` or `gui/src/streamsession.cpp`.

Operator mode: env `NAX5_OPERATOR_MODE=1` (or `NAX5_OPERATOR_BUILD`). Operator QSettings are `NAX5/NAX5-Operator` and may use the original chiaki registration flow. Product mode uses `NAX5/NAX5` and must not write `RegisteredHost` / `ManualHost` for Play.

## How Play reaches Remote Play

`Nax5SessionController` reserves a Session, fetches connection material, then calls `nax5FillStreamSessionConnectInfo()` and `QmlBackend::createSession()`. That reuses chiaki-ng `StreamSession`. There is no NAX5 decoder or relay.

Material fields used: host, target, nickname, `regist_key`, `morning`, optional PIN. Keys are overwritten then cleared on `discardMaterial()` / logout. Do not log them.

## Video decoder

The decoder is chiaki-ng's setting (Settings → Video → Hardware Decoder), default `auto`. On Windows `auto`
tries `vulkan` first and falls back to `d3d11va` where Vulkan video decoding is not available. Some GPU
drivers crash inside their Vulkan decoder (seen in the field: Intel `igvk64.dll`, every session of one
player); `d3d11va` works on the same machines. The stream log records the decoder actually used
(`Using hardware decoder "..."`); BUILD-INFO records only the setting. The replay test covers Vulkan only on
machines where it is available.

## Console power

Before a stream the launcher sends the Remote Play wake-up packet. That brings the console back from rest
mode. A console that is powered off does not answer it, and nothing in the launcher can switch it on. A
player can power the shared console off from the console's own menu during a session; the launcher cannot
prevent or detect that. The launcher itself never sends the console to sleep in product mode.

## Play time (build 16)

When `/api/v1/auth/me/` reports `billingEnforced`, the play panel shows `balanceSeconds`, heartbeats carry
`remainingSeconds`, and a closing 404 carries `reason`. All of these fields are optional: without them the
launcher behaves like build 15. The server decides everything about time; the launcher only shows it. Details
in [docs/SESSION-LIFECYCLE.md](docs/SESSION-LIFECYCLE.md) and [docs/MESSAGES.md](docs/MESSAGES.md).

## Play eligibility

Product Play requires authenticated user, verified email, and `accessStatus == ACTIVE`. Backend reserve also accepts `INVITED`; the client gate is stricter until product UX aligns.

## Logging

| Layer | Path / endpoint |
| --- | --- |
| Process log | `%AppData%/Roaming/NAX5/NAX5/log/nax5_<timestamp>.log` |
| Stream log | same dir, `chiaki_session_<timestamp>.log` (last 5 kept) |
| Manual ZIP | Desktop via `Nax5Session.saveReport()` |
| Automatic report journal | `%AppData%/Roaming/NAX5/NAX5/log/report-queue-v2/`; sanitized, owner-tagged session ZIP parts |
| Server reports | `POST https://cloudgta6.com/api/v1/client-reports/` after a session, when logged in |
| Crash dumps | `%AppData%/Roaming/NAX5/NAX5/log/crash-dumps/NAX5-<utc>-<pid>.dmp` + `.txt`; uploaded as kind `crash` on the next login |

### Crash dumps

`nax5InstallCrashHandler()` runs at the top of `main()`. On an unhandled
exception, `abort()` or `std::terminate()` a pre-started thread writes a
minidump (stacks, threads, modules; no heap, so tokens and Remote Play keys
stay out) and `crash-summary` text with the exception code, faulting module and
offset. Windows Error Reporting still runs afterwards, so the crash also lands
in the Application event log (ID 1000). Admin-mode installs additionally set
WER `LocalDumps` for `chiaki.exe` into the same folder; duplicates of a crash
the handler already caught are dropped.

On the next login `nax5QueueCrashDumps()` turns each crash into one `crash`
report (summary + dump, or summary only when the dump exceeds the 2 MiB cap)
attributed to the play session that was running. Release builds have no DWARF
but keep the COFF symbol table, so map an offset with the exact build's
`chiaki.exe`:

```
scripts/diagnostics/symbolize-crash.sh chiaki.exe 0x1A2B3C
```

Automatic reports contain the complete sanitized process and stream logs captured
for that session, split into bounded ZIP parts below the server limit. Every
journal and part persists the authenticated numeric owner ID; queue selection
filters by that owner. Parts remain until the server acknowledges them. The reconstruction
tool and exact field semantics are documented in
[docs/DIAGNOSTICS-BUILD7.md](docs/DIAGNOSTICS-BUILD7.md).

## Critical invariants

- Do not change Remote Play protocol, video decode/render, audio, controller/input, frame pacing, codec logic, crypto, streaming networking, or renderer/libplacebo without a dedicated architectural decision. See [FORK-MAINTENANCE.md](FORK-MAINTENANCE.md).
- Product-mode disconnect/stop must **not** send the shared PS5 to sleep. `GoToBed()` is operator-mode only (`QmlBackend::closeRequested` / suspend).
- New product features must not break vanilla Remote Play behavior used for diagnostics.
- Session token stays in RAM (`Nax5AuthController::clearSessionToken`). Closing the app requires a new login.
- Do not persist Play connection material in Chiaki host lists.

## Tests

CMake unit tests:

- `gui/src/nax5/nax5authparser_test.cpp`
- `gui/src/nax5/session/nax5sessionparser_test.cpp`
- `gui/src/nax5/connection/nax5connection_test.cpp`
- `gui/src/nax5/nax5telemetry_test.cpp`

Beyond the unit tests: the client and backend integration test
(`scripts/tests/integration/run-backend-integration.ps1`, real HTTP code against a local backend) and the
stream replay matrix ([scripts/tests/stream-replay/README.md](scripts/tests/stream-replay/README.md)).
Neither opens the product window: new on-screen elements still need a look on a real machine.

Manual product checks: [docs/acceptance/ALPHA05-USER-TEST.md](docs/acceptance/ALPHA05-USER-TEST.md) and [TEST-MATRIX.md](TEST-MATRIX.md). Older alpha notes are under [docs/acceptance/history/](docs/acceptance/history/).

## Packaging

Full guide: [docs/release/BUILD-LAUNCHER.md](docs/release/BUILD-LAUNCHER.md)

```bash
# one-time
bash scripts/release/setup-msys2-build-env.sh

# MSYS2 MINGW64, clean committed tree
bash scripts/release/build-alpha05-user-pack.sh
```

Output:

- `artifacts/alpha-0.5/NAX5-windows-installer.exe` (primary user download)
- `artifacts/alpha-0.5/NAX5-Alpha-0.5-Windows-x64.zip` (portable fallback for release hosting)

Inno Setup 6 (`ISCC.exe`) is required. Operator/local `.cmd` helpers are **not** included in the user pack.

Record releases in workspace `RELEASES.md` and publish assets to GitHub Releases.
