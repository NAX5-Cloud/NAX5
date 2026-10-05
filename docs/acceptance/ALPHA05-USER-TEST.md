# NAX5 Alpha 0.5 user test

Production user pack for Windows x64. Default API: `https://cloudgta6.com`.

Build with `scripts/release/build-alpha05-user-pack.sh`. Canonical user artifact: `artifacts/alpha-0.5/NAX5-windows.zip`, containing one `NAX5.exe`.

## Install

1. Download the canonical `NAX5-windows.zip` from the GitHub release (`alpha-0.5-build-4`).
2. Extract the ZIP — it contains one `NAX5.exe` (Inno Setup installer).
3. Run `NAX5.exe`, finish setup, launch NAX5. Do not set `NAX5_API_BASE_URL` or `NAX5_OPERATOR_MODE`.
4. Do not register a PS5, supply PSN tokens, use a manual IP/PIN, configure port forwarding, or request sleep for the shared console.
4. SmartScreen may warn on the unsigned build — expected. Stop if Defender reports a real threat.

## Account

Register on [cloudgta6.com/register/](https://www.cloudgta6.com/register/), confirm email, wait for **`ACTIVE`** access status.
Play is gated on `ACTIVE` everywhere (website copy, backend reserve, client Play button).

> Written for build 4. The checklist below still applies, but the release tag, the report contents (complete
> logs in parts, not a tail) and the heartbeat have changed since; see [../CHANGELOG.md](../CHANGELOG.md).

## Logs

| Location | Files |
| --- | --- |
| `%AppData%\Roaming\NAX5\NAX5\log\` | `nax5_*.log` (control plane), `chiaki_session_*.log` (stream) |
| Desktop ZIP | «Сохранить отчёт» — last 5 session logs + current process log |
| Server | `POST /api/v1/client-reports/` on apex — tail only, when logged in |

## AUTOMATED (required before tagging)

Run before tagging. The release script runs the first three units; run the telemetry unit separately from the same configured build directory:

- `nax5-auth-unit`, `nax5-session-unit`, `nax5-connection-unit`, `nax5-telemetry-unit` — **PASS required**
- `verify-qml-product-mode.ps1`, `verify-no-host-persistence.ps1`, `verify-settings-contract.ps1`, `verify-start-nax5-local.ps1`
- Portable tree checks (`chiaki.exe`, Qt DLLs, `qt.conf`, no `.cmd`, no operator files)
- `FileDescription` = `NAX5 Remote Play Client`
- ZIP integrity (`unzip -tq`)

```bash
cmake --build build-alpha05 --target nax5-telemetry-unit
build-alpha05/gui/nax5-telemetry-unit.exe
```

Record automated run:

| Field | Value |
| --- | --- |
| Date (UTC) | |
| Client SHA | |
| Host / toolchain | MSYS2 MINGW64 |
| Result | PASS / FAIL |

## MANUAL — product (production)

Do not mark PASS without user confirmation on a clean Windows 11 PC + physical PS5.

| # | Scenario | Status | Notes |
| --- | --- | --- | --- |
| 1 | Login with verified **ACTIVE** account | **NOT TESTED** | |
| 2 | Play → reserve → automatic Remote Play (no Register / IP / PIN / PSN Account ID) | **NOT TESTED** | |
| 3 | Video, audio, controller, fullscreen | **NOT TESTED** | |
| 4 | Disconnect → Play again | **NOT TESTED** | |
| 5 | 20–30 minute soak | **NOT TESTED** | |
| 6 | User B → `NO_CAPACITY` while User A occupies console | **NOT TESTED** | |
| 7 | User A release → User B reserve succeeds | **NOT TESTED** | |
| 8 | WAN / mobile hotspot from another network | **NOT TESTED** | |
| 9 | «Сохранить отчёт» → ZIP on Desktop + server report when authenticated | **NOT TESTED** | |
| 10 | Client quit frees server session (no orphan ACTIVE) | **NOT TESTED** | |
| 11 | PS5 does not sleep from product client disconnect | **NOT TESTED** | |
| 12 | Second launch does not require manual PS5 registration | **NOT TESTED** | |

### Manual session record (fill after real test)

| Field | Value |
| --- | --- |
| Date (UTC) | |
| Tester | |
| Client SHA | |
| Backend SHA | |
| Web SHA | |
| Network | LAN / WAN / hotspot |
| PS5 firmware | |
| Overall | GO / NO-GO |

## MANUAL — operator (separate build)

Operator pack is **not** included in the Alpha 0.5 user ZIP. Provision consoles via SSH-tunneled operator client; see backend `docs/DEPLOYMENT.md`.

Physical PS5 PIN registration and READY activation are operator-only historical procedures; see [ALPHA04-USER-TEST.md](../archive/acceptance/ALPHA04-USER-TEST.md). They are not user-test instructions.

## NOT IN SCOPE

- Code signing / SmartScreen reputation
- Auto-update
- `INVITED` Play gate (requires `ACTIVE`)
