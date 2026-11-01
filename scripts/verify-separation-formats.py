#!/usr/bin/env python3
"""Compare SliceLab's JUCE decoder to the separation decoder on synthetic audio.

Build scripts/verify-juce-audio.cpp against the existing SliceLab shared-code
archive, then pass that executable with --juce-probe. All generated files and
the JSON report go below the explicitly chosen --output directory.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import wave
from pathlib import Path

import numpy as np
import scipy.signal
import soundfile as sf


def run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(args, check=True, capture_output=True, text=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--juce-probe", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    target = args.output.resolve()
    target.mkdir(parents=True, exist_ok=True)
    probe = args.juce_probe.resolve()

    # An asymmetric deterministic signal makes any delay observable by
    # cross-correlation. No user audio is used.
    rate, frames = 48000, 144000
    rng = np.random.default_rng(192837)
    t = np.arange(frames) / rate
    audio = np.zeros((frames, 2), np.float64)
    audio[:, 0] = .03 * np.sin(2 * np.pi * (230 * t + 37 * t * t))
    audio[:, 1] = .02 * np.sin(2 * np.pi * (383 * t + 23 * t * t))
    for start, length, gain in [(2400, 511, .4), (18321, 733, .25), (35214, 487, .3), (59271, 1201, .2), (88241, 921, .35), (130027, 731, .22)]:
        noise = rng.standard_normal(length) * np.hanning(length) * gain
        audio[start:start + length, 0] += noise
        audio[start:start + length, 1] -= .67 * noise
    wav = target / "śpiew 日本語 source.wav"
    with wave.open(str(wav), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes((np.clip(audio, -1, .9999) * 32767).astype("<i2").tobytes())

    codecs = {"aiff": ["-c:a", "pcm_s24be"], "flac": ["-c:a", "flac"],
              "ogg": ["-c:a", "libvorbis", "-q:a", "5"],
              "mp3": ["-c:a", "libmp3lame", "-b:a", "128k"]}
    paths = {"wav": wav}
    for ext, settings in codecs.items():
        path = wav.with_suffix("." + ext)
        run("ffmpeg", "-nostdin", "-v", "error", "-y", "-i", str(wav), *settings, str(path))
        paths[ext] = path

    report = {"source_sample_rate": rate, "source_frames": frames,
              "libsndfile_version": sf.__libsndfile_version__,
              "ffmpeg_version": run("ffmpeg", "-version").stdout.splitlines()[0], "formats": {}}
    for ext, path in paths.items():
        juce_pcm = target / (ext + ".juce.f32")
        ffmpeg_pcm = target / (ext + ".ffmpeg.f32")
        metadata = run(str(probe), str(path), str(juce_pcm)).stdout.strip()
        juce_meta = dict(part.split("=", 1) for part in metadata.split())
        run("ffmpeg", "-nostdin", "-v", "error", "-y", "-i", str(path), "-map", "0:a:0", "-vn", "-f", "f32le", str(ffmpeg_pcm))
        juce = np.fromfile(juce_pcm, dtype="<f4").reshape(-1, int(juce_meta["channels"]))[:, 0]
        decoded = np.fromfile(ffmpeg_pcm, dtype="<f4").reshape(-1, int(juce_meta["channels"]))[:, 0]
        # Search ±4096 samples to capture encoder/decoder delay. Use the first
        # second, containing several deterministic noise landmarks.
        a, b = juce[:rate], decoded[:rate]
        corr = scipy.signal.correlate(a, b, mode="full", method="fft")
        lags = scipy.signal.correlation_lags(len(a), len(b), mode="full")
        subset = np.abs(lags) <= 4096
        lag = int(lags[subset][np.argmax(corr[subset])])
        jstart, fstart = max(lag, 0), max(-lag, 0)
        common = min(len(juce) - jstart, len(decoded) - fstart)
        correlation = float(np.corrcoef(juce[jstart:jstart + common], decoded[fstart:fstart + common])[0, 1])
        info = sf.info(path)
        report["formats"][ext] = {
            "source_name": path.name, "source_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "juce_frames": int(juce_meta["frames"]), "juce_sample_rate": int(float(juce_meta["sample_rate"])),
            "juce_channels": int(juce_meta["channels"]), "juce_read_succeeded": juce_meta["decoded"] == "1",
            "libsndfile_frames": info.frames, "libsndfile_sample_rate": info.samplerate,
            "libsndfile_channels": info.channels, "ffmpeg_decoded_frames": len(decoded),
            "juce_lag_vs_ffmpeg_frames": lag, "aligned_correlation": correlation,
        }
    destination = target / "comparison.json"
    destination.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
