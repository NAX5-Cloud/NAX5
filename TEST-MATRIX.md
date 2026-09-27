# TEST MATRIX

Permanent NAX5 test register. Status is automated unless marked Manual. Current target: Alpha 0.5 build 4 (`alpha-0.5-build-4`).

| Test | Automated/Manual | Backend/NAX5/Vanilla/Both | LAN/WAN | Commit | Status | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Env absent → production HTTPS | Automated | NAX5 | n/a | | Automated implemented | `nax5-auth-unit` |
| `NAX5_API_BASE_URL=http://127.0.0.1:8000` | Automated | NAX5 | LAN | | Automated implemented | exact localhost URL |
| Localhost HTTP allowed | Automated | NAX5 | LAN | | Automated implemented | |
| Non-local HTTP rejected | Automated | NAX5 | n/a | | Automated implemented | |
| Malformed URL controlled error | Automated | NAX5 | n/a | | Automated implemented | |
| Trailing slash normalization | Automated | NAX5 | n/a | | Automated implemented | |
| Spaces/invalid env | Automated | NAX5 | n/a | | Automated implemented | |
| No silent fallback to production | Automated | NAX5 | n/a | | Automated implemented | |
| `start-nax5-local.cmd` helper | Automated | NAX5 | LAN | | Automated implemented | `scripts/nax5/verify-start-nax5-local.ps1` |
| User A reserve / User B NO_CAPACITY | Automated+Manual | Both | LAN | | Automated PASS historically; User B manual PASS 2026-09-10 | |
| User A release / User B reserve | Manual | Both | LAN | | **Not user-confirmed** | Task 3 gate C |
| Auth unit | Automated | NAX5 | n/a | | Implemented | `nax5-auth-unit` |
| Session unit | Automated | NAX5 | n/a | | Implemented | `nax5-session-unit` |
| Connection unit | Automated | NAX5 | n/a | | Implemented | `nax5-connection-unit` |
| Telemetry unit | Automated | NAX5 | n/a | | Implemented | `nax5-telemetry-unit`; installation/version/SHA fields and secret screening |
| Settings contract (Settings* → StreamSessionConnectInfo) | Automated | NAX5 | n/a | | Source contract script | `verify-settings-contract.ps1` |
| Product QML hides Register/Consoles/PSN/export | Automated | NAX5 | n/a | | Source contract script | `verify-qml-product-mode.ps1` |
| No host persistence in NAX5 adapter | Automated | NAX5 | n/a | | Source contract script | `verify-no-host-persistence.ps1` |
| QSettings product vs operator namespace | Automated | NAX5 | n/a | | Connection unit | Product `NAX5`, Operator `NAX5-Operator` |
| Connection Base64 / length / version | Automated | Both | n/a | | Implemented | 16-byte `CHIAKI_SESSION_AUTH_SIZE` |
| Connection retry CONNECTING | Automated | Backend | n/a | | Implemented | same console, same session |
| Owner mismatch / unauthenticated | Automated | Backend | n/a | | Implemented | 401 / 403 |
| ACTIVE does not reissue material | Automated | Backend | n/a | | Implemented | |
| Operator staff provision/activate | Automated | Backend | n/a | | Implemented | non-staff 403 |
| Secret provider atomic replace | Automated | Backend | n/a | | Implemented | filesystem provider |
| Django accounts/waitlist/consoles/sessions | Automated | Backend | n/a | | Run against PostgreSQL | never SQLite |
| Allocator concurrency | Automated | Backend | n/a | | Existing Task 3 suite | |
| Stream replay matrix (decoders, codecs incl. HDR, mic/echo, loss, corruption, jitter, stall) | Automated | NAX5 | n/a | | 17/17 PASS 2026-09-27 | `scripts/tests/stream-replay/`; needs `-DNAX5_STREAM_REPLAY=ON` build; Vulkan decoder only where available |
| Launcher Qt offscreen smoke | Automated | NAX5 | n/a | | Limited | requires packaged `chiaki.exe` + Qt offscreen |
| Operator physical PIN + READY | Manual | Both | LAN | | **STOP here** | do not simulate PS5 |
| Product 0 registered hosts automatic play | Manual | NAX5 | LAN/WAN | | **NOT TESTED** | Alpha 0.5 user pack |
| Client-reports upload on quit/manual | Manual | NAX5 | WAN | | **NOT TESTED** | apex `/api/v1/client-reports/` |
| Telemetry event payload privacy | Automated | NAX5 | n/a | | Implemented | no password, session token, registration key, or morning value |
| Process + session log rotation | Automated | NAX5 | n/a | | Implemented | `nax5processlog`, 5 files |
| Vanilla A/B 5–10 min | Manual | Both | LAN/WAN | | After user connect | same PC/PS5/controller |
| 30+ minute soak | Manual | NAX5 | LAN/WAN | | **NOT TESTED** | heartbeat is Task 6; hard TTL 7200s |
