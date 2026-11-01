#!/usr/bin/env python3
"""Separate one WAV on the configured SliceLab GPU host."""
from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time

SCHEMA = 1
PIPELINE = "slicelab-bs-roformer-v1.2.0"
MODEL = "model_bs_roformer_ep_17_sdr_9.6568.ckpt"
MODEL_SHA256 = "3e9daecd70aaed5b5a0d1f861cc4d77eaa45afb3fc6301b1cf32c1be0f5868fb"
CONFIG_SHA256 = "d8afb980318d0c08b9c2e24a7adc00d4f3150320c127a7e4de861800d1321939"
STEMS = ("vocals", "drums", "bass", "other")
RUNTIME = {"audio_separator": "0.47.0", "torch": "2.13.0+cu130", "numpy": "2.4.6", "scipy": "1.17.1", "soundfile": "0.14.0", "libsndfile": "1.2.2", "librosa": "0.11.0", "soxr": "1.1.0", "einops": "0.8.2", "rotary_embedding_torch": "0.6.5", "ffmpeg": "6.1.1-3ubuntu5", "device": "cuda"}
SETTINGS = {"model_sample_rate": 44100, "overlap": 8, "batch_size": 1, "pitch_shift": 0, "normalization_threshold": 1.0, "precision": "fp32", "augmentations": ["original", "channel_swap", "polarity_invert"], "output": "pcm_s24le"}
CACHE = Path(os.environ.get("SLICELAB_STEM_CACHE", "~/.cache/slicelab/stems")).expanduser()


class WorkerError(Exception):
    pass


class Cancelled(Exception):
    pass


def atomic_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    temporary.write_text(json.dumps(payload, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def wav_info(path: Path) -> tuple[int, int, int]:
    with path.open("rb") as stream:
        header = stream.read(12)
        if len(header) != 12 or header[:4] != b"RIFF" or header[8:] != b"WAVE":
            raise WorkerError("output is not a RIFF WAV file")
        file_size = path.stat().st_size
        riff_size = struct.unpack_from("<I", header, 4)[0] + 8
        if riff_size > file_size:
            raise WorkerError("truncated WAV file")
        fmt = None
        data_size = None
        while stream.tell() + 8 <= min(file_size, riff_size):
            chunk = stream.read(8)
            chunk_size = struct.unpack_from("<I", chunk, 4)[0]
            end = stream.tell() + chunk_size
            if end > min(file_size, riff_size):
                raise WorkerError("truncated WAV chunk")
            if chunk[:4] == b"fmt ":
                raw = stream.read(min(chunk_size, 40))
                if len(raw) < 16:
                    raise WorkerError("invalid WAV format")
                encoding, channels, rate, _, align, bits = struct.unpack_from("<HHIIHH", raw)
                if encoding == 0xFFFE:
                    if len(raw) < 40:
                        raise WorkerError("invalid extensible WAV format")
                    encoding = struct.unpack_from("<H", raw, 24)[0]
                if encoding != 1 or bits != 24 or channels not in (1, 2) or rate <= 0 or align != channels * 3:
                    raise WorkerError("output WAV must be mono or stereo 24-bit PCM")
                fmt = (rate, channels, align)
            elif chunk[:4] == b"data":
                if data_size is not None:
                    raise WorkerError("WAV contains multiple data chunks")
                data_size = chunk_size
            stream.seek(end + (chunk_size & 1))
        if not fmt or data_size is None or data_size % fmt[2]:
            raise WorkerError("WAV audio data is invalid")
        return fmt[0], fmt[1], data_size // fmt[2]


def result_key(input_hash: str) -> str:
    return hashlib.sha256(f"{PIPELINE}\n{MODEL_SHA256}\n{CONFIG_SHA256}\n{input_hash}\n".encode()).hexdigest()


def validate_manifest(manifest: dict, job_id: str, input_hash: str) -> None:
    expected = {"schema": SCHEMA, "worker_version": "1.2.0", "job_id": job_id, "input_sha256": input_hash, "model": MODEL, "model_sha256": MODEL_SHA256, "config_sha256": CONFIG_SHA256}
    if not isinstance(manifest, dict) or any(manifest.get(k) != v for k, v in expected.items()):
        raise WorkerError("remote result identity does not match the requested model and input")
    for key in ("sample_rate", "channels", "frames"):
        if type(manifest.get(key)) is not int or manifest[key] <= 0:
            raise WorkerError(f"invalid remote {key}")
    if manifest["channels"] not in (1, 2):
        raise WorkerError("remote output has an unsupported channel count")
    if manifest.get("runtime") != RUNTIME or manifest.get("settings") != SETTINGS:
        raise WorkerError("remote runtime or inference settings do not match the pinned recipe")
    files = manifest.get("files")
    if not isinstance(files, dict) or set(files) != set(STEMS):
        raise WorkerError("remote result must contain exactly four named stems")
    for stem in STEMS:
        item = files[stem]
        if not isinstance(item, dict) or item.get("filename") != stem + ".wav" or type(item.get("size")) is not int or item["size"] <= 44 or not re.fullmatch(r"[a-f0-9]{64}", str(item.get("sha256"))):
            raise WorkerError(f"invalid remote file metadata for {stem}")


def validate_files(manifest: dict, directory: Path) -> None:
    expected = (manifest["sample_rate"], manifest["channels"], manifest["frames"])
    for stem in STEMS:
        item = manifest["files"][stem]
        path = directory / item["filename"]
        if not path.is_file() or path.stat().st_size != item["size"] or sha256(path) != item["sha256"]:
            raise WorkerError(f"checksum verification failed for {stem}")
        if wav_info(path) != expected:
            raise WorkerError(f"format or frame count verification failed for {stem}")


def run_command(arguments: list[str], timeout: float = 30) -> str:
    with tempfile.TemporaryFile() as output, tempfile.TemporaryFile() as error:
        process = subprocess.Popen(arguments, stdin=subprocess.DEVNULL, stdout=output, stderr=error)
        try:
            code = process.wait(timeout=timeout)
        except BaseException:
            process.kill()
            process.wait()
            raise
        output.seek(0)
        stdout = output.read(256 * 1024 + 1)
        error.seek(0, os.SEEK_END)
        error.seek(max(0, error.tell() - 2000))
        stderr = error.read().decode("utf-8", "replace").strip()
        if code:
            raise WorkerError(f"{arguments[0]} failed: {stderr or 'exit ' + str(code)}")
        if len(stdout) > 256 * 1024:
            raise WorkerError(f"{arguments[0]} returned too much output")
        return stdout.decode("utf-8", "replace")


def ssh(host: str, command: str, job_id: str, input_hash: str | None = None) -> dict:
    worker = os.environ.get("SLICELAB_REMOTE_WORKER")
    executable = shlex.quote(worker) if worker else '"$HOME/.local/share/slicelab-stems/v1/remote_worker.py"'
    arguments = [command, "--job-id", job_id]
    if input_hash:
        arguments += ["--input-sha256", input_hash]
    forwarded = [f"{key}={os.environ[key]}" for key in ("SLICELAB_REMOTE_ROOT", "SLICELAB_REMOTE_MODELS", "SLICELAB_SEPARATION_PYTHON") if key in os.environ]
    remote = ("env " + shlex.join(forwarded) + " " if forwarded else "") + "python3 " + executable + " " + shlex.join(arguments)
    try:
        response = run_command(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15", host, remote])
        result = json.loads(response)
        if not isinstance(result, dict):
            raise ValueError("not an object")
        return result
    except (WorkerError, subprocess.TimeoutExpired, OSError, ValueError) as error:
        raise WorkerError(f"Remote request failed. If sign-in expired, run ssh {host} in a terminal and retry. {str(error)[:1200]}") from error


def scp(source: str, destination: str, host: str) -> None:
    try:
        run_command(["scp", "-q", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15", source, destination], timeout=1800)
    except (WorkerError, subprocess.TimeoutExpired, OSError) as error:
        raise WorkerError(f"Transfer failed. If sign-in expired, run ssh {host} in a terminal and retry. {str(error)[:1200]}") from error


def local_result(manifest: dict, directory: Path, input_path: Path) -> dict:
    return {"schema": SCHEMA, "input_sha256": manifest["input_sha256"], "model": MODEL, "model_sha256": MODEL_SHA256, "sample_rate": manifest["sample_rate"], "frames": manifest["frames"], "channels": manifest["channels"], "stems": {name: str(directory / f"{name}.wav") for name in STEMS}, "pipeline": PIPELINE, "config_sha256": CONFIG_SHA256, "source_path": str(input_path), "runtime": manifest.get("runtime"), "settings": manifest.get("settings"), "processing_gain": manifest.get("processing_gain"), "provenance": manifest.get("provenance"), "files": manifest["files"]}


def update_status(path: Path, state: str, detail: str, **fields) -> None:
    atomic_json(path, {"state": state, "detail": detail[:1500], "progress": 1.0 if state == "complete" else None, **fields})


def snapshot_input(source: Path, cache: Path) -> tuple[Path, str]:
    cache.mkdir(parents=True, exist_ok=True)
    before = source.stat()
    if not source.is_file() or before.st_size <= 44:
        raise WorkerError("input is missing or empty")
    fd, name = tempfile.mkstemp(prefix=".input-", suffix=".wav", dir=cache)
    snapshot = Path(name)
    digest = hashlib.sha256()
    try:
        with os.fdopen(fd, "wb") as destination, source.open("rb") as original:
            for block in iter(lambda: original.read(1024 * 1024), b""):
                digest.update(block)
                destination.write(block)
        after = source.stat()
        if (before.st_ino, before.st_size, before.st_mtime_ns) != (after.st_ino, after.st_size, after.st_mtime_ns):
            raise WorkerError("input changed while it was being read; retry")
        return snapshot, digest.hexdigest()
    except BaseException:
        snapshot.unlink(missing_ok=True)
        raise


def separate(source: Path, result_path: Path, status_path: Path, host: str) -> None:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", host):
        raise WorkerError("host must be an SSH host alias")
    if not source.is_absolute() or not result_path.is_absolute() or not status_path.is_absolute():
        raise WorkerError("input, result, and status paths must be absolute")
    source, result_path, status_path = source.resolve(), result_path.resolve(), status_path.resolve()
    if len({source, result_path, status_path}) != 3:
        raise WorkerError("input, result, and status paths must be different")
    update_status(status_path, "preparing", "Reading the selected audio")
    result_path.unlink(missing_ok=True)
    cache = CACHE.resolve()
    snapshot, input_hash = snapshot_input(source, cache)
    job_id = result_key(input_hash)
    directory = cache / job_id
    started_here = False
    try:
        with (cache / f".{job_id}.lock").open("a+") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            manifest_path = directory / "manifest.json"
            try:
                manifest = json.loads(manifest_path.read_text())
                validate_manifest(manifest, job_id, input_hash)
                validate_files(manifest, directory)
            except (OSError, ValueError, WorkerError):
                manifest = None
            if manifest is not None:
                atomic_json(result_path, local_result(manifest, directory, source))
                update_status(status_path, "complete", "Using verified cached stems", input_sha256=input_hash, job_id=job_id, cached=True)
                return
            update_status(status_path, "connecting", "Connecting to the GPU host", input_sha256=input_hash, job_id=job_id)
            prepared = ssh(host, "prepare", job_id)
            remote_path = prepared.get("path")
            if not isinstance(remote_path, str) or not re.fullmatch(r"/[A-Za-z0-9_./-]+", remote_path) or any(part in (".", "..") for part in remote_path.split("/")) or not remote_path.endswith("/" + job_id):
                raise WorkerError("remote job path is invalid")
            if prepared.get("state") not in ("processing", "complete"):
                update_status(status_path, "uploading", "Uploading selected audio", input_sha256=input_hash, job_id=job_id)
                scp(str(snapshot), f"{host}:{remote_path}/input.upload", host)
                ssh(host, "publish", job_id, input_hash)
            launched = ssh(host, "start", job_id, input_hash)
            if launched.get("state") == "needs_upload":
                update_status(status_path, "uploading", "Uploading selected audio", input_sha256=input_hash, job_id=job_id)
                scp(str(snapshot), f"{host}:{remote_path}/input.upload", host)
                ssh(host, "publish", job_id, input_hash)
                launched = ssh(host, "start", job_id, input_hash)
            if launched.get("state") not in ("processing", "complete"):
                raise WorkerError("remote worker could not start the job")
            started_here = launched.get("started") is True
            update_status(status_path, "processing", "Separating four stems on the remote GPU", input_sha256=input_hash, job_id=job_id)
            failures = 0
            while True:
                try:
                    current = ssh(host, "status", job_id)
                    failures = 0
                except WorkerError:
                    failures += 1
                    if failures >= 3:
                        raise
                    time.sleep(2)
                    continue
                state = current.get("state")
                if state == "complete":
                    break
                if state in ("error", "cancelled", "missing"):
                    raise WorkerError(str(current.get("detail", "Remote worker did not complete"))[:1200])
                if state != "processing":
                    raise WorkerError("Remote worker returned an invalid state")
                update_status(status_path, "processing", str(current.get("detail", "Separating four stems on the remote GPU")), input_sha256=input_hash, job_id=job_id)
                time.sleep(2)
            remote_manifest = ssh(host, "manifest", job_id)
            validate_manifest(remote_manifest, job_id, input_hash)
            update_status(status_path, "downloading", "Downloading and verifying four stems", input_sha256=input_hash, job_id=job_id)
            temporary = Path(tempfile.mkdtemp(prefix=f".{job_id}.", dir=cache))
            try:
                for stem in STEMS:
                    scp(f"{host}:{remote_path}/output/{stem}.wav", str(temporary / f"{stem}.wav"), host)
                validate_files(remote_manifest, temporary)
                atomic_json(temporary / "manifest.json", remote_manifest)
                if directory.exists():
                    shutil.rmtree(directory)
                os.replace(temporary, directory)
            finally:
                if temporary.exists():
                    shutil.rmtree(temporary)
            atomic_json(result_path, local_result(remote_manifest, directory, source))
            update_status(status_path, "complete", "Four verified stems are ready", input_sha256=input_hash, job_id=job_id, cached=False)
    except Cancelled:
        remote_state = "not_started_here"
        if started_here:
            try:
                remote_state = str(ssh(host, "cancel", job_id).get("state", "unknown"))[:80]
            except WorkerError:
                remote_state = "unreachable"
        detail = "Local request cancelled; the remote job may continue"
        if remote_state == "cancelling":
            detail = "Local request cancelled; remote cancellation requested"
        elif remote_state in ("cancelled", "complete"):
            detail = f"Local request cancelled; remote job is {remote_state}"
        update_status(status_path, "cancelled", detail, input_sha256=input_hash, job_id=job_id, remote_state=remote_state)
        raise
    finally:
        snapshot.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    command = sub.add_parser("separate")
    command.add_argument("--input", required=True, type=Path)
    command.add_argument("--result", required=True, type=Path)
    command.add_argument("--status", required=True, type=Path)
    command.add_argument("--host", default="wojtek-ai-pc")
    command.add_argument("--quality", default="best", choices=("best",))
    args = parser.parse_args()
    def interrupted(_signum, _frame):
        raise Cancelled()
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    try:
        separate(args.input, args.result, args.status, args.host)
        return 0
    except Cancelled:
        return 130
    except (WorkerError, OSError, ValueError, subprocess.TimeoutExpired) as error:
        detail = str(error)[:1500]
        try:
            if args.status.is_absolute() and args.status.resolve() != args.input.resolve():
                update_status(args.status, "error", detail)
        except OSError:
            pass
        print(f"STEM_ERROR: {detail}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
