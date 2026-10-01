#!/usr/bin/env python3
"""Generate replay media for the NAX5 stream replay test.

Each output directory holds what the PS5 would deliver to chiaki's decoder:
  video.au    access units, each framed as [u32 LE size][Annex-B bytes]
  audio.opus  Opus packets (48 kHz, stereo, 10 ms), same framing
  meta.json   {"codec": "h264|h265|h265_hdr", "width", "height", "fps"}

Encoding mirrors Remote Play: no B-frames, low-latency, one IDR at the start and
a long GOP, bitrate in the range chiaki requests. Needs ffmpeg with libx264,
libx265 and libopus (MSYS2 mingw-w64-x86_64-ffmpeg has them).

Usage: make-replay-media.py <out-root> [--seconds 10]
"""
import argparse
import json
import pathlib
import struct
import subprocess
import sys

PROFILES = {
    # name: codec, width, height, fps, kbps
    "h264-720p60": ("h264", 1280, 720, 60, 10000),
    "h264-1080p60": ("h264", 1920, 1080, 60, 15000),
    "h264-720p30": ("h264", 1280, 720, 30, 6000),
    "h265-1080p60": ("h265", 1920, 1080, 60, 15000),
    "h265hdr-1080p60": ("h265_hdr", 1920, 1080, 60, 15000),
}


def run(cmd):
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        sys.stderr.write(result.stderr.decode("utf-8", "replace")[-4000:])
        raise SystemExit(f"command failed: {' '.join(cmd[:6])} ...")


def nal_starts(data):
    """Offsets of Annex-B start codes (position of the first 0x00)."""
    i, n = 0, len(data)
    while i + 3 <= n:
        j = data.find(b"\x00\x00\x01", i)
        if j < 0:
            return
        yield j - 1 if j > 0 and data[j - 1] == 0 else j, j + 3
        i = j + 3


def split_access_units(data, codec):
    """Split an Annex-B stream at access unit delimiters."""
    cuts = []
    for start, header in nal_starts(data):
        if header >= len(data):
            break
        byte = data[header]
        is_aud = (byte & 0x1F) == 9 if codec == "h264" else ((byte >> 1) & 0x3F) == 35
        if is_aud:
            cuts.append(start)
    cuts.append(len(data))
    return [data[a:b] for a, b in zip(cuts, cuts[1:]) if b > a]


def ogg_packets(data):
    """Opus packets from an Ogg stream, skipping OpusHead/OpusTags."""
    packets, pending, pos = [], b"", 0
    while pos + 27 <= len(data):
        if data[pos:pos + 4] != b"OggS":
            raise SystemExit("bad ogg page")
        segments = data[pos + 26]
        table = data[pos + 27:pos + 27 + segments]
        body = pos + 27 + segments
        for lace in table:
            pending += data[body:body + lace]
            body += lace
            if lace < 255:
                packets.append(pending)
                pending = b""
        pos = body
    return [p for p in packets if not p.startswith((b"OpusHead", b"OpusTags"))]


def framed(chunks):
    return b"".join(struct.pack("<I", len(c)) + c for c in chunks)


def make_video(out, codec, width, height, fps, kbps, seconds):
    raw = out / ("video.h264" if codec == "h264" else "video.hevc")
    src = f"testsrc2=size={width}x{height}:rate={fps},noise=alls=12:allf=t"
    common = ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", src, "-t", str(seconds)]
    rate = ["-b:v", f"{kbps}k", "-maxrate", f"{kbps}k", "-bufsize", f"{kbps // 4}k"]
    gop = fps * seconds
    if codec == "h264":
        cmd = common + ["-c:v", "libx264", "-preset", "veryfast", "-tune", "zerolatency", "-profile:v", "high",
                        "-pix_fmt", "yuv420p", "-bf", "0", "-g", str(gop), "-sc_threshold", "0"] + rate + \
              ["-bsf:v", "h264_metadata=aud=insert", "-f", "h264", str(raw)]
    else:
        ten_bit = codec == "h265_hdr"
        params = f"keyint={gop}:min-keyint={gop}:scenecut=0:bframes=0:aud=1:repeat-headers=1"
        if ten_bit:
            params += ":colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc"
        cmd = common + ["-c:v", "libx265", "-preset", "veryfast", "-tune", "zerolatency",
                        "-pix_fmt", "yuv420p10le" if ten_bit else "yuv420p",
                        "-profile:v", "main10" if ten_bit else "main", "-x265-params", params] + rate + \
              ["-f", "hevc", str(raw)]
    run(cmd)
    units = split_access_units(raw.read_bytes(), "h264" if codec == "h264" else "h265")
    raw.unlink()
    if len(units) < fps * seconds * 0.9:
        raise SystemExit(f"{out.name}: only {len(units)} access units, expected ~{fps * seconds}")
    (out / "video.au").write_bytes(framed(units))
    return len(units)


def make_audio(out, seconds):
    ogg = out / "audio.ogg"
    run(["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
         "-i", f"sine=frequency=440:sample_rate=48000:duration={seconds}", "-ac", "2", "-ar", "48000",
         "-c:a", "libopus", "-frame_duration", "10", "-b:a", "96k", "-application", "lowdelay", str(ogg)])
    packets = ogg_packets(ogg.read_bytes())
    ogg.unlink()
    (out / "audio.opus").write_bytes(framed(packets))
    return len(packets)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("out_root")
    parser.add_argument("--seconds", type=int, default=10)
    args = parser.parse_args()
    root = pathlib.Path(args.out_root)
    for name, (codec, width, height, fps, kbps) in PROFILES.items():
        out = root / name
        out.mkdir(parents=True, exist_ok=True)
        units = make_video(out, codec, width, height, fps, kbps, args.seconds)
        packets = make_audio(out, args.seconds)
        (out / "meta.json").write_text(json.dumps({"codec": codec, "width": width, "height": height, "fps": fps}))
        print(f"{name}: {units} access units, {packets} opus packets")


if __name__ == "__main__":
    main()
