# NAX5 fork maintenance

Upstream: [streetpea/chiaki-ng](https://github.com/streetpea/chiaki-ng)

Baseline: chiaki-ng v1.10.0 (`nax5-baseline` is an exact snapshot). Do not commit product patches to that branch.

Product patches:

- P01 Application identity
- P02 Settings isolation
- P03 Windows metadata/package
- P04 NAX5 native backend authentication
- P05 Backend console assignment
- P06 Automatic Remote Play orchestration (transient `StreamSessionConnectInfo`, operator profile)

Streaming core modifications: `lib/` none (identical to v1.10.0). `gui/src/streamsession.cpp` differs from
upstream in the four places listed under "Differences in protected files" below.

Protected areas (do not change unless a dedicated architectural decision requires it):

- `lib/src/session.c`
- `lib/include/chiaki/session.h`
- `gui/src/streamsession.cpp`
- `gui/src/settings.cpp`
- `gui/src/host.cpp`
- Remote Play protocol, decoder, renderer, audio, controller, discovery, and PSN registration

NAX5 C++ product code lives under `gui/include/nax5/` and `gui/src/nax5/`. Session tokens and connection keys stay in RAM. Product mode does not write `RegisteredHost` / `ManualHost` for Play. Operator mode uses QSettings `NAX5/NAX5-Operator` and the original chiaki-ng registration flow.

Developer guide: [NAX5.md](NAX5.md).

## Differences in protected files

Checked against `nax5-baseline` (chiaki-ng v1.10.0) with
`git diff nax5-baseline <ref> -- lib gui/src/streamsession.cpp gui/include/streamsession.h gui/src/settings.cpp gui/src/host.cpp`.
State at build 16: `lib/`, `gui/src/settings.cpp` and `gui/src/host.cpp` are unchanged. `streamsession.cpp` and
its header differ as follows; builds 15 and 16 added nothing here.

| Since | Change | Affects a release build |
| --- | --- | --- |
| build 9 | Read-only getters in `streamsession.h`: initial RTT, MTU in, MTU out (for diagnostics) | yes, read-only |
| build 14 | Echo cancellation: microphone and echo buffers sized from the frame layout instead of `SDL_AudioCVT::len_ratio`, which is garbage with sdl2-compat on Windows and corrupted the heap | yes: audio/microphone code |
| build 14 | A failed session start is retried after `nax5StreamRetryDelayMs()` instead of `SESSION_RETRY_SECONDS / 3` milliseconds (upstream passed 6 ms and sent about 150 session requests in 2 s) | yes: stream start retry |
| build 14 | Hooks for the stream replay test under `#ifdef NAX5_STREAM_REPLAY` (start, stop, microphone frame) | no: compiled out of release builds |

Other upstream GUI files changed outside the product directories: `gui/src/qmlbackend.cpp` and its header
(creates the NAX5 controllers, transient connect info, operator-only sleep), `gui/src/main.cpp` (crash handler,
single instance, product identity), `gui/src/qmlsettings.cpp` (suspend action forced to "nothing" in product
mode), `gui/src/qmlmainwindow.cpp` (window title).
