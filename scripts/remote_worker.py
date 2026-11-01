#!/usr/bin/env python3
from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time

VERSION = "1.2.0"
MODEL = "model_bs_roformer_ep_17_sdr_9.6568.ckpt"
MODEL_SHA256 = "3e9daecd70aaed5b5a0d1f861cc4d77eaa45afb3fc6301b1cf32c1be0f5868fb"
CONFIG = "config_bs_roformer_384_8_2_485100.yaml"
CONFIG_SHA256 = "d8afb980318d0c08b9c2e24a7adc00d4f3150320c127a7e4de861800d1321939"
STEMS = ("vocals", "drums", "bass", "other")
RUNTIME = {"audio_separator": "0.47.0", "torch": "2.13.0+cu130", "numpy": "2.4.6", "scipy": "1.17.1", "soundfile": "0.14.0", "libsndfile": "1.2.2", "librosa": "0.11.0", "soxr": "1.1.0", "einops": "0.8.2", "rotary_embedding_torch": "0.6.5", "ffmpeg": "6.1.1-3ubuntu5", "device": "cuda"}
SETTINGS = {"model_sample_rate": 44100, "overlap": 8, "batch_size": 1, "pitch_shift": 0, "normalization_threshold": 1.0, "precision": "fp32", "augmentations": ["original", "channel_swap", "polarity_invert"], "output": "pcm_s24le"}
ROOT = Path(os.environ.get("SLICELAB_REMOTE_ROOT", "~/.cache/slicelab-stems")).expanduser().resolve()
MODEL_DIR = Path(os.environ.get("SLICELAB_REMOTE_MODELS", "~/.local/share/slicelab-stems/v1/models")).expanduser().resolve()
PYTHON = Path(os.environ.get("SLICELAB_SEPARATION_PYTHON", "~/.local/share/reaper-remote-stems/.venv/bin/python")).expanduser()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def atomic_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    tmp.write_text(json.dumps(value, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def job_path(job_id: str) -> Path:
    if not re.fullmatch(r"[a-f0-9]{64}", job_id):
        raise ValueError("invalid job ID")
    root = (ROOT / "jobs").resolve()
    path = (root / job_id).resolve()
    if path.parent != root:
        raise ValueError("invalid job path")
    return path


def running(path: Path) -> bool:
    try:
        pid = int((path / "pid").read_text())
        command = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
        return b"work" in command and str(path.name).encode() in command and str(Path(__file__).resolve()).encode() in command
    except (OSError, ValueError):
        return False


def status(path: Path) -> dict:
    if running(path):
        try:
            data = json.loads((path / "status.json").read_text())
            if data.get("state") not in ("complete", "error", "cancelled"):
                return data
        except (OSError, ValueError):
            pass
        return {"state": "processing", "detail": "Starting the remote worker"}
    try:
        data = json.loads((path / "status.json").read_text())
    except (OSError, ValueError):
        return {"state": "missing", "detail": "No remote job is running"}
    if data.get("state") not in ("complete", "error", "cancelled"):
        return {"state": "error", "detail": "Remote worker stopped before completing"}
    return data


def checked_assets() -> None:
    for name, expected in ((MODEL, MODEL_SHA256), (CONFIG, CONFIG_SHA256)):
        path = MODEL_DIR / name
        if not path.is_file() or sha256(path) != expected:
            raise RuntimeError(f"Missing or incorrect pinned model asset: {path}")


def completed_result_valid(path: Path, input_hash: str) -> bool:
    try:
        manifest = json.loads((path / "manifest.json").read_text())
        expected = {"schema": 1, "worker_version": VERSION, "job_id": path.name, "input_sha256": input_hash, "model": MODEL, "model_sha256": MODEL_SHA256, "config_sha256": CONFIG_SHA256}
        if any(manifest.get(key) != value for key, value in expected.items()) or manifest.get("runtime") != RUNTIME or manifest.get("settings") != SETTINGS:
            return False
        files = manifest["files"]
        if set(files) != set(STEMS):
            return False
        for stem in STEMS:
            item = files[stem]
            output = path / "output" / f"{stem}.wav"
            if item["filename"] != output.name or not output.is_file() or output.stat().st_size != item["size"] or sha256(output) != item["sha256"]:
                return False
        return True
    except (OSError, AttributeError, KeyError, TypeError, ValueError):
        return False


def start(path: Path, input_hash: str) -> dict:
    if not re.fullmatch(r"[a-f0-9]{64}", input_hash):
        raise ValueError("invalid input checksum")
    path.mkdir(parents=True, exist_ok=True)
    with (path / "launch.lock").open("a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if running(path):
            return {"started": False, "state": "processing"}
        current = status(path)
        if current["state"] == "complete" and completed_result_valid(path, input_hash):
            return {"started": False, "state": "complete"}
        checked_assets()
        source = path / "input.wav"
        if not source.is_file() or sha256(source) != input_hash:
            return {"started": False, "state": "needs_upload"}
        if not PYTHON.is_file():
            raise RuntimeError(f"separation Python is missing: {PYTHON}")
        atomic_json(path / "status.json", {"state": "processing", "detail": "Starting the remote GPU worker"})
        log = (path / "worker.log").open("wb")
        environment = {**os.environ, "PYTHONDONTWRITEBYTECODE": "1"}
        process = subprocess.Popen([str(PYTHON), str(Path(__file__).resolve()), "work", "--job-id", path.name, "--input-sha256", input_hash], stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, start_new_session=True, close_fds=True, env=environment)
        (path / "pid").write_text(str(process.pid))
        log.close()
        return {"started": True, "state": "processing"}


def publish(path: Path, input_hash: str) -> dict:
    if not re.fullmatch(r"[a-f0-9]{64}", input_hash):
        raise ValueError("invalid input checksum")
    with (path / "launch.lock").open("a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if running(path):
            return {"state": "processing"}
        upload = path / "input.upload"
        if not upload.is_file() or sha256(upload) != input_hash:
            raise RuntimeError("uploaded input checksum does not match")
        os.replace(upload, path / "input.wav")
        return {"state": "uploaded"}


def cancel(path: Path) -> dict:
    with (path / "launch.lock").open("a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if running(path):
            os.killpg(int((path / "pid").read_text()), signal.SIGTERM)
            return {"state": "cancelling"}
        return status(path)


def work(path: Path, input_hash: str) -> None:
    def interrupted(_signum, _frame):
        raise InterruptedError("Separation was cancelled")
    signal.signal(signal.SIGTERM, interrupted)
    try:
        import importlib.metadata
        import numpy as np
        import soundfile as sf
        import torch
        from audio_separator.separator import Separator

        versions = {name.replace("-", "_"): importlib.metadata.version(name) for name in ("audio-separator", "numpy", "scipy", "soundfile", "librosa", "soxr", "einops", "rotary-embedding-torch")}
        ffmpeg = subprocess.run(["ffmpeg", "-version"], check=True, capture_output=True, text=True, timeout=10).stdout.splitlines()[0].split()[2]
        versions.update(torch=torch.__version__, libsndfile=sf.__libsndfile_version__, ffmpeg=ffmpeg, device="cuda")
        if versions != RUNTIME:
            raise RuntimeError(f"Remote runtime differs from the pinned recipe: {versions}")
        if not torch.cuda.is_available():
            raise RuntimeError("CUDA is unavailable on the remote machine")
        checked_assets()
        source = path / "input.wav"
        if sha256(source) != input_hash:
            raise RuntimeError("remote input checksum changed")
        info = sf.info(source)
        if info.frames <= 0 or info.samplerate <= 0 or info.channels not in (1, 2):
            raise RuntimeError("Input must be a nonempty mono or stereo WAV file")
        work_dir = path / "work"
        output_dir = path / "output"
        for folder in (work_dir, output_dir):
            if folder.exists():
                shutil.rmtree(folder)
            folder.mkdir()
        model_input = work_dir / "model_input.wav"
        subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-y", "-i", str(source), "-map", "0:a:0", "-vn", "-ar", "44100", "-ac", "2", "-c:a", "pcm_f32le", str(model_input)], check=True, timeout=1800)
        model_info = sf.info(model_input)
        atomic_json(path / "status.json", {"state": "processing", "detail": "Separating four stems on the remote GPU"})

        class PinnedSeparator(Separator):
            def download_model_files(self, model_filename):
                self.model_is_uvr_vip = False
                return model_filename, "MDXC", "ZFTurbo BS-RoFormer", str(MODEL_DIR / MODEL), CONFIG

        separator = PinnedSeparator(model_file_dir=str(MODEL_DIR), output_dir=str(work_dir), output_format="WAV", use_soundfile=True, normalization_threshold=1.0, sample_rate=44100, mdxc_params={"segment_size": 256, "override_model_segment_size": False, "batch_size": 1, "overlap": 8, "pitch_shift": 0})
        separator.load_model(MODEL)
        if str(separator.torch_device).split(":")[0] != "cuda":
            raise RuntimeError("Separator did not select CUDA")
        if separator.effective_precision != "fp32":
            raise RuntimeError("Separator did not select full precision")
        audio, model_rate = sf.read(model_input, dtype="float32", always_2d=True)
        if model_rate != 44100 or audio.shape != (model_info.frames, 2) or not np.isfinite(audio).all():
            raise RuntimeError("decoded model input is invalid")
        peak = float(np.max(np.abs(audio)))
        input_gain = 1.0 / peak if peak > 1.0 else 1.0
        mix = audio.T.copy()
        if input_gain != 1.0:
            mix *= input_gain
        del audio
        engine = separator.model_instance
        override = engine._use_model_segment_override(model_info.frames / 44100)
        sources = engine.demix(mix, override_model_segment_size=override)
        if not isinstance(sources, dict) or set(sources) != set(STEMS):
            raise RuntimeError("model did not produce the required four named stems")
        for pass_number, label in enumerate(("channel swap", "polarity inversion"), 2):
            augmented = mix[::-1].copy() if label == "channel swap" else -mix
            atomic_json(path / "status.json", {"state": "processing", "detail": f"Separating four stems on the remote GPU, pass {pass_number} of 3"})
            estimate = engine.demix(augmented, override_model_segment_size=override)
            if not isinstance(estimate, dict) or set(estimate) != set(STEMS):
                raise RuntimeError("augmentation did not produce the required four named stems")
            for stem in STEMS:
                if estimate[stem].shape != (2, model_info.frames) or sources[stem].shape != (2, model_info.frames):
                    raise RuntimeError(f"{stem} model output does not align with the model input")
                if label == "channel swap":
                    sources[stem] += estimate[stem][::-1]
                else:
                    sources[stem] -= estimate[stem]
            del estimate, augmented
        for stem in STEMS:
            sources[stem] /= 3.0
            if not np.isfinite(sources[stem]).all():
                raise RuntimeError(f"{stem} model output contains invalid samples")
        peak = max(float(np.max(np.abs(sources[stem]))) for stem in STEMS)
        output_gain = 1.0 / peak if peak > 1.0 else 1.0
        for stem in STEMS:
            sf.write(work_dir / f"{stem}.wav", (sources[stem] * output_gain).T, 44100, subtype="FLOAT")
        atomic_json(path / "status.json", {"state": "processing", "detail": "Checking stem lengths and formats"})
        files = {}
        for stem in STEMS:
            raw = work_dir / f"{stem}.wav"
            raw_info = sf.info(raw)
            if raw_info.samplerate != 44100 or raw_info.channels != 2 or raw_info.frames != model_info.frames:
                raise RuntimeError(f"{stem} model output does not align with the model input")
            target = output_dir / f"{stem}.wav"
            subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-y", "-i", str(raw), "-af", f"aresample={info.samplerate},apad,atrim=end_sample={info.frames}", "-ac", str(info.channels), "-c:a", "pcm_s24le", str(target)], check=True, timeout=1800)
            out_info = sf.info(target)
            if (out_info.samplerate, out_info.channels, out_info.frames) != (info.samplerate, info.channels, info.frames):
                raise RuntimeError(f"{stem} output format or frame count is incorrect")
            files[stem] = {"filename": target.name, "sha256": sha256(target), "size": target.stat().st_size}
        manifest = {"schema": 1, "worker_version": VERSION, "job_id": path.name, "input_sha256": input_hash, "model": MODEL, "model_sha256": MODEL_SHA256, "config_sha256": CONFIG_SHA256, "sample_rate": info.samplerate, "channels": info.channels, "frames": info.frames, "files": files, "runtime": versions, "settings": SETTINGS, "processing_gain": {"input": input_gain, "output": output_gain}, "provenance": {"author": "ZFTurbo", "release": "https://github.com/ZFTurbo/Music-Source-Separation-Training/releases/tag/v1.0.12", "weight_license_clarification": "https://github.com/ZFTurbo/Music-Source-Separation-Training/issues/254", "augmentation": "https://github.com/ZFTurbo/Music-Source-Separation-Training/blob/main/utils/model_utils.py"}}
        atomic_json(path / "manifest.json", manifest)
        atomic_json(path / "status.json", {"state": "complete", "detail": "Four stems are ready"})
    except BaseException as error:
        state = "cancelled" if isinstance(error, (InterruptedError, KeyboardInterrupt)) else "error"
        atomic_json(path / "status.json", {"state": state, "detail": f"{type(error).__name__}: {str(error)[:1200]}"})
        raise


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("prepare", "publish", "start", "status", "manifest", "cancel", "work"))
    parser.add_argument("--job-id", required=True)
    parser.add_argument("--input-sha256")
    args = parser.parse_args()
    try:
        path = job_path(args.job_id)
        if args.command == "prepare":
            path.mkdir(parents=True, exist_ok=True)
            result = {"path": str(path), **status(path)}
        elif args.command == "publish":
            result = publish(path, args.input_sha256 or "")
        elif args.command == "start":
            result = start(path, args.input_sha256 or "")
        elif args.command == "status":
            result = status(path)
        elif args.command == "manifest":
            if status(path)["state"] != "complete":
                raise RuntimeError("remote result is not complete")
            result = json.loads((path / "manifest.json").read_text())
        elif args.command == "cancel":
            result = cancel(path)
        else:
            work(path, args.input_sha256 or "")
            result = {"state": "complete"}
        print(json.dumps(result, sort_keys=True))
        return 0
    except Exception as error:
        print(f"REMOTE_ERROR: {type(error).__name__}: {str(error)[:1200]}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
