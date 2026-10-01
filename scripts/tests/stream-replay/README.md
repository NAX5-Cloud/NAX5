# Stream replay test

Runs the real stream window (decoder, libplacebo renderer, audio output,
microphone, echo cancellation) on recorded media instead of a PS5. Catches
crashes and starved decoders before a build ships.

The replay code exists only when CMake is configured with
`-DNAX5_STREAM_REPLAY=ON`. Release builds use `OFF` (see
`scripts/release/build-alpha05-user-pack.sh`), so product binaries never
contain it.

## Run

```bash
# 1. Media (once): H.264/HEVC/HEVC-HDR like a PS5 sends, Opus 48 kHz 10 ms
python3 scripts/tests/stream-replay/make-replay-media.py /c/astro/worktrees/nax5-replay-media

# 2. Replay-enabled client in its own build dir
cmake -S . -B ../worktrees/nax5-client-replay-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCHIAKI_ENABLE_CLI=OFF -DNAX5_STREAM_REPLAY=ON
cmake --build ../worktrees/nax5-client-replay-build --target chiaki
```

```powershell
# 3. Matrix (about 8 minutes)
$b = 'C:\astro\worktrees\nax5-client-replay-build'
scripts\tests\stream-replay\run-stream-replay.ps1 -ChiakiExe "$b\gui\chiaki.exe" `
  -MediaRoot C:\astro\worktrees\nax5-replay-media `
  -DllDir "$b\third-party\cpp-steam-tools","C:\msys64\mingw64\bin"
```

For a packaged build, point `-ChiakiExe` at the bundle's `chiaki.exe` and drop
`-DllDir`. `-Scenario name,...` and `-Repeat N` narrow or repeat runs.

## What a scenario checks

- the process exits cleanly (no crash code, no hang, no crash dump);
- decoded frames >= 95% of accepted access units (50% with loss or corruption);
- the audio device opens; with the microphone on, mic frames flow unless the
  machine has no capture device (reported as `no-device`);
- a decoder that is unavailable on the machine is reported as a fallback, not
  as coverage.

Runs use operator mode (`NAX5-Operator` profile) and a dead API URL, so no
data reaches production. Per-run logs, results and crash dumps are copied to
`-OutDir`.

## Environment the client reads

`NAX5_REPLAY_DIR` (media set), `NAX5_REPLAY_SECONDS`, `NAX5_REPLAY_DECODER`
(`auto`, `none`, `d3d11va`, `vulkan`, ...), `NAX5_REPLAY_MIC`, `NAX5_REPLAY_ECHO`,
`NAX5_REPLAY_LOSS_PCT`, `NAX5_REPLAY_CORRUPT_PCT`, `NAX5_REPLAY_BURST_EVERY_S`,
`NAX5_REPLAY_STALL_MS`, `NAX5_REPLAY_AUDIO_HEADER_ONLY`, `NAX5_REPLAY_RESULT`.

## Found so far

- 2026-09-27: microphone with echo cancellation corrupted the heap on every
  played audio frame (sdl2-compat `SDL_AudioCVT` packing mismatch made the echo
  buffer 0 bytes). Scenario `mic-on-echo`: 6/6 crashes before the fix, 0 after.
