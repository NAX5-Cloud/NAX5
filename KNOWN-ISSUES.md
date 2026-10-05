# KNOWN ISSUES

Findings from the Task 4.1 implementation and audit. RAM extraction of connection material is Task 5, not a Task 4 defect.

| ID | Severity | Component | Description | Reproduction | Impact | Root cause | Fix now / defer | Target |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| KI-001 | P1 | Task 3 manual | User A release → User B reserve is not user-confirmed | Two local accounts, one READY console | Capacity gate incomplete | Manual step not executed | Confirm before Alpha 0.5 WAN test | Alpha 0.5 |
| KI-011 | fixed Alpha 0.5 | Product gate | Eligibility unified: verified email + `ACTIVE` for reserve/Play | INVITED user logs in | Play disabled; reserve 403 | Client gate extended to backend + site copy | Deploy backend/web RC | Alpha 0.5 |
| KI-002 | TASK5 SECURITY RISK | NAX5 RAM | `registKey` / morning exist in process memory for the stream lifetime | Product Play after connection | User with local debugger can dump keys | Required for libchiaki session | Defer. Do not obfuscate | Task 5 |
| KI-003 | P2 | Vanilla parity | Public WAN IP uses remote 720p profile | `isLocalAddress(host)` false | Lower WAN resolution vs LAN | Upstream `StreamSessionConnectInfo` | Do not change in Task 4 | later quality task |
| KI-004 | P2 | Packaging | Clean Release portable and vanilla control build were not produced in this session | No `chiaki.exe` in the worktree | Manual operator test needs a package | MSYS2 build not run here | Build before physical PIN | Task 4 package |
| KI-005 | P3 | QML smoke | Headless Qt offscreen launcher smoke is not runnable without a built GUI binary | `QT_QPA_PLATFORM=offscreen` | Missing runtime QML crash net | No current GUI binary | Source-level QML assertions in place | Task 4 package |
| KI-006 | P3 | Operator UX | Operator provision uses the first operator `RegisteredHost` if display index 0 is empty | Operator panel Provision | Wrong host if several are registered | Smallest adapter | Operator should keep one lab host | later |
| KI-007 | P3 | Auth | Operator HTTP uses the same `csrf_exempt` native token pattern as Task 3 | Browser cookie session against operator URL | Not a browser app API | Alpha native client | Documented; staff still required | Task 5 if browser admin is added |
| KI-008 | fixed build 6 | Lifecycle | Client did not heartbeat ACTIVE sessions (it does since build 6: every 20 s, server lease 60 s) | Long play past 7200s | Server expires CONNECTING/ACTIVE after `SESSION_HARD_TIMEOUT_SECONDS` | Heartbeat is Task 6 | Temporary hard TTL only | Task 6 |
| KI-012 | fixed | Product session | Second Play never posted `/connected/` so CONNECTING expired at 180s while video was live | Replay after a successful stream | `played_seconds=0`, `LEASE_EXPIRED_DURING_ALLOCATION` | `stream_first_frame_seen` stayed true across generations | Reset first-frame + bind `stream_generation` before `createSession` | Alpha 0.5 |
| KI-009 | P3 | Git | Nested `test/munit` worktree can make `git status` fail in this checkout | `git status` | Fork diff budget harder to compute | Nested baseline worktree | Use pathspec or a clean clone for budget | maintenance |
| KI-010 | P3 | Settings | Product Config tab remains (About / non-secret profile UI); credential export/import is hidden | Open Settings in Product | Extra upstream page still listed | Hide capability, not delete upstream | Acceptable for Alpha | later |
| KI-013 | mitigated build 16 | Decoder | Launcher crashes inside the GPU's Vulkan driver (`igvk64.dll`, access violation) when `auto` picks the `vulkan` decoder | Intel integrated graphics, default settings, start a stream | Every session of the affected player crashes within minutes | Driver bug on the Vulkan video path; `d3d11va` works | Build 16 switches to `d3d11va` after the first such crash; the first crash itself still happens | build 16 |
| KI-014 | P2 | Crash handler | Crash report arrives with `dump_omitted: missing`: summary written, minidump not | Crash under heavy packet loss (fault in `ntdll.dll`) | The crash cannot be located | Unknown; the process kept running for about a minute after the exception | Build 16 records `dump_result` in the summary to tell a failed write from a hung one; the cause is still open | after build 16 |
| KI-015 | fixed build 16 | Reserve | After «no console» the launcher blocks «Играть» for 5 s but the server refuses for 60 s; every press uploads a `reserve-fail` report | Press «Играть» repeatedly while the console is busy | «Слишком много попыток» for a minute; hundreds of noise reports per player | Client pause was not raised with the server's | Align the pause, no report on 429 | build 16 |
| KI-016 | P1 | Console power | The launcher can wake the console from rest mode but cannot switch on a powered-off console; a player can power it off from the console menu | Choose «Turn off PS5» on the console during a session | The console stays unavailable for everybody until somebody switches it on by hand; players see «Не удалось подключиться к консоли» | Remote Play has no power-on for a console that is off | The site agent reports the console's state and the backend stops handing out a switched-off console and mails the owner; build 16 shows `CONSOLE_OFFLINE` and asks players not to power it off. Switching it back on is still manual | backend + site agent, build 16 |
| KI-017 | P2 | Crash under loss | Access violation in `ntdll.dll` at one offset during 25-30 % packet loss with `d3d11va`, decoder buffer full | Very lossy link | Launcher exits | Not located (no dump, see KI-014); probably in the decode path of corrupted video | Needs a dump first; if it is in the core, report upstream | later |
| KI-018 | P3 | Admin semantics | A new «Играть» during a live session closes the old one as `FAILED / CONNECTION_FAILED` | Crash, restart, press «Играть» within 60 s | Admin shows connection failures that are not | The old session is ended by the new reserve | Separate reason code on the backend | backend |

Fixed during Task 4.1 (not open):

- `lifecycle.py` `elif` without `if` (would not import)
- Operator test payload `session_id=operator-test` rejected by product session match
- Product `goToSleep()` could still send shared PS5 to sleep
- `syncCurrent` could abort an in-flight connection request
- SessionQuit during user cancel could mark FAILED
- Invalid env API URL no longer falls back to production
