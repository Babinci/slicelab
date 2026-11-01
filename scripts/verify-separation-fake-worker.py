#!/usr/bin/env python3
"""Deterministic local worker used by verify-separation-integration.cpp only."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import time
import wave


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def wav_chunks(path):
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("expected RIFF WAV")
    chunks = {}
    offset = 12
    while offset + 8 <= len(data):
        name, size = struct.unpack_from("<4sI", data, offset)
        offset += 8
        chunks[name] = data[offset:offset + size]
        offset += size + (size & 1)
    fmt = chunks[b"fmt "]
    encoding, channels, rate, _, alignment, bits = struct.unpack_from("<HHIIHH", fmt)
    if encoding == 0xfffe:
        encoding = struct.unpack_from("<H", fmt, 24)[0]
    return encoding, channels, rate, alignment, bits, chunks[b"data"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("command")
    for flag in ("input", "result", "status", "host", "quality"):
        parser.add_argument("--" + flag, required=True)
    args = parser.parse_args()
    source = Path(args.input)
    directory = Path(os.environ["SLICELAB_FAKE_DIR"])
    directory.mkdir(parents=True, exist_ok=True)
    mode = os.environ.get("SLICELAB_FAKE_MODE", "normal")
    marker = directory / "called.json"
    encoding, channels, rate, alignment, bits, data = wav_chunks(source)
    if mode == "expected-original":
        if source.resolve() != Path(os.environ["SLICELAB_FAKE_ORIGINAL"]).resolve():
            raise ValueError("WAV source path changed")
    else:
        if (encoding, channels, bits) != (3, 2, 32):
            raise ValueError("canonical input must be stereo IEEE float32 WAV")
        if data != Path(os.environ["SLICELAB_FAKE_EXPECTED_PCM"]).read_bytes():
            raise ValueError("canonical PCM differs from the loaded JUCE audio")
    frames = len(data) // alignment
    marker.write_text(json.dumps({"input": str(source), "sha256": sha(source), "frames": frames,
                                  "rate": rate, "channels": channels, "encoding": encoding, "bits": bits}))
    if mode == "wait":
        time.sleep(30)
    if mode == "modify-source":
        with Path(os.environ["SLICELAB_FAKE_ORIGINAL"]).open("ab") as stream:
            stream.write(b"changed")
    stems = {}
    files = {}
    for name in ("vocals", "drums", "bass", "other"):
        target = directory / (name + ".wav")
        output_frames = frames - 1 if mode == "bad-stem-frames" and name == "vocals" else frames
        with wave.open(str(target), "wb") as out:
            out.setnchannels(channels)
            out.setsampwidth(3)
            out.setframerate(rate)
            out.writeframes(bytes(output_frames * channels * 3))
        stems[name] = str(target)
        files[name] = {"filename": target.name, "size": target.stat().st_size, "sha256": sha(target)}
    result = {"schema": 1, "input_sha256": "0" * 64 if mode == "bad-transport-hash" else sha(source),
              "source_path": str(source), "sample_rate": rate, "channels": channels, "frames": frames,
              "model": "test-only", "model_sha256": "0" * 64, "stems": stems, "files": files,
              "processing_gain": {"input": 1.0, "output": 1.0}}
    Path(args.result).write_text(json.dumps(result))
    Path(args.status).write_text(json.dumps({"state": "complete", "detail": "fixture ready"}))


if __name__ == "__main__":
    main()
