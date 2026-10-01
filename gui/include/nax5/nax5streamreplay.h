#pragma once

// Test-only stream replay (CMake option NAX5_STREAM_REPLAY, never in product
// builds). Replaces the PS5 network side of a StreamSession with recorded video
// access units and Opus audio, so the real decoder, renderer, audio output and
// microphone paths run without a console. Driven by NAX5_REPLAY_* environment
// variables; see scripts/tests/stream-replay/README.md.

class StreamSession;
struct StreamSessionConnectInfo;

bool nax5StreamReplayActive();
// Applies decoder/codec/fps/microphone overrides from the environment.
void nax5StreamReplayApply(StreamSessionConnectInfo &info);
void nax5StreamReplayStart(StreamSession *session);
void nax5StreamReplayStop(StreamSession *session);
// Called instead of sending an encoded microphone frame to the console.
void nax5StreamReplayMicFrame();
