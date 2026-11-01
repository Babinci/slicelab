#!/usr/bin/env python3
"""Render SliceLab fixtures in a private REAPER instance and check the WAVs."""
from __future__ import annotations

import argparse
import base64
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import sys
import threading
import time
import uuid
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parent.parent
VERIFY = ROOT / "local/verification"
REAPER = Path("/home/wojtek/opt/REAPER/reaper")
MAGIC = struct.pack("<I", 0x21324356)


def xml_state(data: bytes) -> tuple[int, int, ET.Element]:
    position = data.find(MAGIC)
    if position < 0 or data.find(MAGIC, position + 4) >= 0:
        raise ValueError("VST3 chunk does not contain exactly one JUCE state")
    length = struct.unpack_from("<I", data, position + 4)[0]
    end = position + 8 + length + 1
    if end > len(data) or data[end - 1] != 0:
        raise ValueError("JUCE XML state has an invalid length or terminator")
    return position, end, ET.fromstring(data[position + 8:end - 1])


def replace_component(template: bytes, fixture: bytes) -> bytes:
    position, end, _ = xml_state(template)
    begin2, end2, _ = xml_state(fixture)
    if begin2 != 0:
        raise ValueError("fixture is not a raw JUCE component state")
    component = fixture[:end2]
    prefix = bytearray(template[:position])
    if position:
        if position != 8:
            raise ValueError(f"unexpected VST3 component offset {position}")
        old_size = struct.unpack_from("<I", prefix, 0)[0]
        if old_size < end - position or position + old_size > len(template):
            raise ValueError("unrecognized VST3 component size field")
        struct.pack_into("<I", prefix, 0, old_size + len(component) - (end - position))
    return bytes(prefix) + component + template[end:]


def check_restored(expected: bytes, actual: bytes) -> None:
    _, _, a = xml_state(expected)
    _, _, b = xml_state(actual)
    for key in ("source", "sourceHash", "stemManifest"):
        if a.get(key, "") != b.get(key, ""):
            raise ValueError(f"restored state changed {key}")
    pads_a = [(p.get("index"), float(p.get("start")), float(p.get("end"))) for p in a.find("PADS")]
    pads_b = [(p.get("index"), float(p.get("start")), float(p.get("end"))) for p in b.find("PADS")]
    if len(pads_a) != len(pads_b) or any(
        i != j or not math.isclose(x, u, abs_tol=3e-5) or not math.isclose(y, v, abs_tol=3e-5)
        for (i, x, y), (j, u, v) in zip(pads_a, pads_b)
    ):
        raise ValueError("restored pad boundaries differ")
    values_a = {p.get("id"): float(p.get("value")) for p in a.find("PARAMETERS")}
    values_b = {p.get("id"): float(p.get("value")) for p in b.find("PARAMETERS")}
    if values_a.keys() != values_b.keys():
        raise ValueError("restored parameter set differs")
    mismatches = [k for k in values_a if not math.isclose(values_a[k], values_b[k], abs_tol=0.001)]
    if mismatches:
        raise ValueError("restored values differ for " + ", ".join(mismatches[:8]))


def state_with_overrides(fixture: bytes, changes: dict, pad_end: float | None = None,
                         pad_bounds: list | None = None, source: Path | None = None) -> bytes:
    _, _, root = xml_state(fixture)
    parameters = {parameter.get("id"): parameter for parameter in root.find("PARAMETERS")}
    for key, value in changes.items():
        if key not in parameters:
            raise ValueError("missing parameter " + key)
        parameters[key].set("value", str(value))
    if "repitch" in changes:
        root.set("repitch", "1" if changes["repitch"] else "0")
    if pad_end is not None:
        pads = [pad for pad in root.find("PADS") if pad.get("index") == "0"]
        if len(pads) != 1:
            raise ValueError("fixture does not contain exactly one first pad")
        pads[0].set("end", str(pad_end))
    if pad_bounds is not None:
        pads = {int(pad.get("index")): pad for pad in root.find("PADS")}
        if len(pads) != len(pad_bounds) or set(pads) != set(range(len(pad_bounds))):
            raise ValueError("fixture pad slots do not match the requested bounds")
        for index, (start, end) in enumerate(pad_bounds):
            pads[index].set("start", str(start))
            pads[index].set("end", str(end))
    if source is not None:
        source = source.resolve(strict=True)
        root.set("source", str(source))
        root.set("sourceHash", hashlib.sha256(source.read_bytes()).hexdigest())
        root.set("stemManifest", "")
    xml = ET.tostring(root, encoding="utf-8")
    return MAGIC + struct.pack("<I", len(xml)) + xml + b"\0"


def lua_value(value) -> str:
    if isinstance(value, str):
        return json.dumps(value, ensure_ascii=False)
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return repr(value)
    if isinstance(value, list):
        return "{" + ",".join(lua_value(v) for v in value) + "}"
    if isinstance(value, dict):
        return "{" + ",".join("[" + lua_value(k) + "]=" + lua_value(v) for k, v in value.items()) + "}"
    raise TypeError(type(value))


def verify_child(process: subprocess.Popen, profile: Path) -> dict:
    proc = Path("/proc") / str(process.pid)
    status = (proc / "status").read_text()
    parent = int(next(line.split()[1] for line in status.splitlines() if line.startswith("PPid:")))
    executable = (proc / "exe").resolve(strict=True)
    arguments = (proc / "cmdline").read_bytes().decode().strip("\0").split("\0")
    if (parent != os.getpid() or executable != REAPER.resolve() or "-newinst" not in arguments or
            "-cfgfile" not in arguments or str(profile / "reaper.ini") not in arguments):
        raise RuntimeError("refusing to signal a process that is not the isolated REAPER child")
    return {"pid": process.pid, "parent_pid": parent, "executable": str(executable), "arguments": arguments}


def stop_child(process: subprocess.Popen, profile: Path) -> None:
    if process.poll() is not None:
        return
    verify_child(process, profile)
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        verify_child(process, profile)
        process.kill()
        process.wait()


def ensure_profile_unused(profile: Path) -> None:
    config = str(profile / "reaper.ini")
    for process in Path("/proc").iterdir():
        if not process.name.isdigit():
            continue
        try:
            if (process / "exe").resolve(strict=True) != REAPER.resolve():
                continue
            arguments = (process / "cmdline").read_bytes().decode().rstrip("\0").split("\0")
            if "-cfgfile" in arguments and config in arguments:
                raise RuntimeError("the private REAPER profile is already in use")
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue


def await_owned_child(process: subprocess.Popen, profile: Path) -> dict:
    deadline = time.monotonic() + 2
    last_error = None
    while time.monotonic() < deadline and process.poll() is None:
        try:
            return verify_child(process, profile)
        except (RuntimeError, FileNotFoundError, ProcessLookupError) as exc:
            last_error = exc
            time.sleep(0.02)
    raise RuntimeError("could not establish isolated REAPER child identity: " + str(last_error))


def start_job(job: dict, profile: Path, run: Path, timeout: float, keep_open: bool = False,
              on_ready=None) -> int:
    ensure_profile_unused(profile)
    name = job["mode"] + "-" + job["name"]
    jobfile = run / "jobs" / (name + ".lua")
    jobfile.write_text("return " + lua_value(job) + "\n")
    env = os.environ.copy()
    env["SLICELAB_REAPER_JOB"] = str(jobfile)
    env["LV2_PATH"] = str(run / "empty-plugins")
    env["CLAP_PATH"] = str(run / "empty-plugins")
    if job.get("pipewire"):
        env["PIPEWIRE_QUANTUM"] = f'{job.get("quantum", 512)}/48000'
        env["LD_LIBRARY_PATH"] = "/usr/${LIB}/pipewire-0.3/jack" + (
            ":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
    command = [str(REAPER), "-newinst", "-cfgfile", str(profile / "reaper.ini"), "-nosplash",
               job["project"], str(ROOT / "tests/reaper_acceptance.lua")]
    with (run / "jobs" / (name + ".log")).open("wb") as log:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                   start_new_session=keep_open)
        process_record = await_owned_child(process, profile)
        (run / "jobs" / (name + ".process.json")).write_text(json.dumps(process_record, indent=2) + "\n")
        deadline = time.monotonic() + timeout
        result = Path(job["result"])
        mapped = set()

        def capture_mappings() -> None:
            try:
                lines = (Path("/proc") / str(process.pid) / "maps").read_text().splitlines()
                for line in lines:
                    fields = line.split(maxsplit=5)
                    if len(fields) == 6 and fields[5].startswith("/") and "SliceLab" in fields[5]:
                        mapped.add(str(Path(fields[5]).resolve()))
            except FileNotFoundError:
                pass

        while not result.exists() and process.poll() is None and time.monotonic() < deadline:
            capture_mappings()
            if on_ready and Path(job["ready"]).exists():
                try:
                    on_ready(process)
                    on_ready = None
                except Exception:
                    stop_child(process, profile)
                    raise
            time.sleep(0.05)
        if result.exists():
            status = result.read_text()
            capture_mappings()
            (run / "jobs" / (name + ".mappings.json")).write_text(json.dumps(sorted(mapped), indent=2) + "\n")
            if not status.startswith("OK\n"):
                stop_child(process, profile)
                raise RuntimeError(status.strip())
            if job.get("plugin_binary") and job["plugin_binary"] not in mapped:
                stop_child(process, profile)
                raise RuntimeError("installed SliceLab binary was not mapped in the REAPER process")
            if not keep_open:
                stop_child(process, profile)
            return process.pid
        else:
            code = process.poll()
            if code is None:
                stop_child(process, profile)
                raise RuntimeError(f"REAPER job timed out after {timeout:g} seconds")
            raise RuntimeError(f"REAPER exited with status {code} before writing job result")


def open_gui(run: Path, timeout: float, pipewire: bool = False, quantum: int = 512,
             source: Path | None = None) -> int:
    run = run.expanduser().resolve(strict=True)
    allowed = (VERIFY / "reaper-runs").resolve()
    if not run.is_relative_to(allowed):
        raise ValueError("GUI project must belong to a private verification run")
    identity = json.loads((run / "identity.json").read_text())
    binary = Path(identity["installed_binary"]).resolve(strict=True)
    if hashlib.sha256(binary.read_bytes()).hexdigest() != identity["sha256_before"]:
        raise ValueError("installed binary changed since this run")
    profile = (VERIFY / "reaper-resource" / run.name).resolve(strict=True)
    name = "inspection-" + uuid.uuid4().hex[:8]
    project = run / "projects" / (name + ".RPP")
    source = (source or run / "projects/sample_original.RPP").expanduser().resolve(strict=True)
    if not source.is_relative_to((run / "projects").resolve()) or source.suffix != ".RPP":
        raise ValueError("GUI source project must be an RPP inside the selected private run")
    shutil.copy2(source, project)
    output = run / "jobs" / ("gui-" + name)
    gui_dir = run / "jobs" / ("gui-snapshots-" + name)
    gui_dir.mkdir()
    job = {"resource": str(profile), "project": str(project), "mode": "gui", "name": name,
           "result": str(output) + ".result", "parameters": str(output) + ".parameters.tsv",
           "metadata": str(output) + ".metadata.tsv", "plugin_binary": str(binary),
           "pipewire": pipewire, "quantum": quantum, "gui_dir": str(gui_dir),
           "gui_request": str(gui_dir / "request")}
    pid = start_job(job, profile, run, timeout, keep_open=True)
    print(json.dumps({"pid": pid, "resource": str(profile), "project": str(project),
                      "binary_sha256": identity["sha256_before"],
                      "requested_jack_quantum": quantum if pipewire else None,
                      "state_snapshot_directory": str(gui_dir)}, indent=2))
    return 0


def inspection_process(folder: Path) -> tuple[Path, dict]:
    folder = folder.expanduser().resolve(strict=True)
    if not folder.is_relative_to((VERIFY / "reaper-runs").resolve()) or not folder.name.startswith("gui-snapshots-inspection-"):
        raise ValueError("snapshot directory must belong to a private inspection session")
    record = folder.parent / (folder.name.replace("gui-snapshots-", "gui-", 1) + ".process.json")
    saved = json.loads(record.read_text())
    process = Path("/proc") / str(saved["pid"])
    arguments = os.fsdecode((process / "cmdline").read_bytes()).rstrip("\0").split("\0")
    stat = (process / "stat").read_text().rsplit(")", 1)[1].split()
    boot = int(next(line.split()[1] for line in Path("/proc/stat").read_text().splitlines() if line.startswith("btime ")))
    started = boot + int(stat[19]) / os.sysconf("SC_CLK_TCK")
    if ((process / "exe").resolve(strict=True) != REAPER.resolve() or arguments != saved["arguments"] or
            (process / "status").stat().st_uid != os.getuid() or abs(record.stat().st_mtime - started) >= 2):
        raise RuntimeError("refusing to use a different REAPER process")
    return folder, saved


def snapshot_gui(folder: Path, timeout: float, save: bool = False) -> int:
    folder, saved = inspection_process(folder)
    process = Path("/proc") / str(saved["pid"])
    request = folder / "request"
    if request.exists():
        raise RuntimeError("an inspection request is already pending")
    token = uuid.uuid4().hex
    temporary = folder / ("request-" + token)
    temporary.write_text(token + (" save\n" if save else " snapshot\n"))
    temporary.rename(request)
    response = folder / (token + ".result")
    deadline = time.monotonic() + timeout
    while not response.exists() and time.monotonic() < deadline:
        if not process.exists():
            raise RuntimeError("the inspection REAPER process exited")
        time.sleep(0.05)
    if not response.exists():
        raise RuntimeError("inspection snapshot timed out")
    status = response.read_text()
    if not status.startswith("OK\n"):
        raise RuntimeError(status.strip())
    data = base64.b64decode((folder / (token + ".b64")).read_bytes(), validate=True)
    _, _, state = xml_state(data)
    values = {p.get("id"): p.get("value") for p in state.find("PARAMETERS")}
    summary = {"token": token, "saved_project": save, "selected_pad": state.get("selectedPad"),
               "source_name": Path(state.get("source", "")).name, "source_sha256": state.get("sourceHash"),
               "stem_manifest_present": bool(state.get("stemManifest")), "view_width": state.get("viewWidth"),
               "view_height": state.get("viewHeight"), "view_start": state.get("viewStart"),
               "view_end": state.get("viewEnd"),
               "pads": [dict(p.attrib) for p in state.find("PADS")], "parameters": values}
    (folder / (token + ".json")).write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps({"token": token, "snapshot": str(folder / (token + ".json")),
                      "saved_project": save}, indent=2))
    return 0


def capture_gui(folder: Path, window: int) -> int:
    folder, saved = inspection_process(folder)
    target = str(window)
    owner = int(subprocess.run(["xdotool", "getwindowpid", target], text=True, check=True,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout.strip())
    if owner != saved["pid"]:
        raise RuntimeError("refusing to capture a window owned by another process")
    token = uuid.uuid4().hex
    raw = folder / (token + ".xwd")
    png = folder / (token + ".png")
    subprocess.run(["xwd", "-silent", "-id", target, "-out", str(raw)], check=True, timeout=10)
    subprocess.run(["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-i", str(raw),
                    "-frames:v", "1", str(png)], check=True, timeout=10)
    print(json.dumps({"window": window, "pid": owner, "image": str(png)}, indent=2))
    return 0


def record_gui(folder: Path, duration: float) -> int:
    folder, saved = inspection_process(folder)
    if not 1 <= duration <= 30:
        raise ValueError("GUI capture duration must be between 1 and 30 seconds")
    objects = json.loads(subprocess.run(["pw-dump"], check=True, stdout=subprocess.PIPE, timeout=10).stdout)
    clients = {str(obj["id"]) for obj in objects if obj.get("type") == "PipeWire:Interface:Client" and
               str(obj.get("info", {}).get("props", {}).get("application.process.id")) == str(saved["pid"])}
    nodes = []
    for obj in objects:
        if obj.get("type") != "PipeWire:Interface:Node":
            continue
        props = obj.get("info", {}).get("props", {})
        if str(props.get("application.process.id")) != str(saved["pid"]) and str(props.get("client.id")) not in clients:
            continue
        outputs = [entry.get("info", {}).get("props", {}) for entry in objects
                   if entry.get("type") == "PipeWire:Interface:Port" and
                   str(entry.get("info", {}).get("props", {}).get("node.id")) == str(obj["id"])]
        ports = {entry.get("port.name") for entry in outputs if entry.get("port.direction") == "out"}
        if {"out1", "out2"}.issubset(ports):
            nodes.append(obj)
    if len(nodes) != 1:
        raise RuntimeError("could not uniquely identify the inspection REAPER PipeWire node")
    node = nodes[0]
    name = node["info"]["props"].get("node.name")
    if not name:
        raise RuntimeError("inspection REAPER does not expose the expected stereo output ports")
    token = uuid.uuid4().hex
    capture_name = "slicelab_gui_" + token[:8]
    audio_path = folder / (token + ".wav")
    with (folder / (token + ".record.log")).open("wb") as log:
        recorder = subprocess.Popen(["pw-record", "--target", "0", "--rate", "48000", "--channels", "2",
            "--format", "f32", "--properties", "{ node.name = " + capture_name + " }", str(audio_path)],
            stdout=log, stderr=subprocess.STDOUT)
        try:
            targets = [capture_name + ":input_FL", capture_name + ":input_FR"]
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                if recorder.poll() is not None:
                    raise RuntimeError("the private GUI recorder exited")
                available = set(subprocess.run(["pw-link", "-i"], text=True, check=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5).stdout.splitlines())
                if set(targets).issubset(available):
                    break
                time.sleep(0.05)
            else:
                raise RuntimeError("the private GUI recorder ports did not appear")
            for source, target in zip((name + ":out1", name + ":out2"), targets):
                subprocess.run(["pw-link", source, target], check=True, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, timeout=5)
            time.sleep(0.8)
            print(json.dumps({"status": "recording", "pid": saved["pid"], "output": str(audio_path),
                              "duration_seconds": duration}), flush=True)
            time.sleep(duration)
        finally:
            end_owned_process(recorder)
    print(json.dumps({"status": "complete", "output": str(audio_path)}), flush=True)
    return 0


def end_owned_process(process: subprocess.Popen) -> None:
    if process.poll() is None:
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def live_capture_effect(gmem: str) -> str:
    return f'''desc:SliceLabCapture
options:gmem={gmem}
in_pin:Left
in_pin:Right
out_pin:Left
out_pin:Right

@init
ext_noinit=1;

@block
playing = play_state & 1;
playing ? (
  n = gmem[1];
  n < 4096 ? (
    at = 100 + n*4;
    gmem[at] = time_precise();
    gmem[at+1] = play_position;
    gmem[at+2] = samplesblock;
    gmem[at+3] = gmem[0];
    gmem[1] = n+1;
  );
);

@sample
playing ? (
  n = gmem[0];
  n < 96000 ? (
    gmem[4] == 0 && (abs(spl0) > 0.00001 || abs(spl1) > 0.00001) ? gmem[4] = time_precise();
    gmem[32768+n*2] = spl0;
    gmem[32769+n*2] = spl1;
    gmem[0] = n+1;
  );
);
'''


def evaluate_live(run: Path, reference: Path, capture_name: str, quantum: int, instrumented: bool,
                  source: str) -> int:
    import numpy as np
    import soundfile as sf

    failures = []
    info = dict(line.split("\t", 1) for line in (run / "live-info.tsv").read_text().splitlines())
    started, stopped = float(info["started"]), float(info["stopped"])
    if not math.isfinite(started) or not math.isfinite(stopped) or stopped <= started:
        failures.append("live playback timestamps are invalid")
    if source == "virtual" and not started < float(info["note_on"]) < float(info["note_off"]) < stopped:
        failures.append("virtual MIDI note-on and note-off were not sent during playback")
    jack, jack_rate = sf.read(run / "jack.wav", dtype="float64", always_2d=True)
    offline, offline_rate = sf.read(reference, dtype="float64", always_2d=True)
    result = {"sample_rate": jack_rate, "reference_rate": offline_rate, "quantum": quantum,
              "instrumented": instrumented, "jack_frames": len(jack),
              "jack_peak": float(np.max(np.abs(jack), initial=0))}
    metadata = dict(line.split("\t", 1) for line in
                    (run / "jobs/live-capture.metadata.tsv").read_text().splitlines())
    try:
        result["reported_pdc_frames"] = int(metadata["pdc_frames"])
        result["reported_pdc_ms"] = 1000 * result["reported_pdc_frames"] / jack_rate
    except (ValueError, KeyError, ZeroDivisionError):
        failures.append("host did not report the plugin delay compensation")
    try:
        result["chain_pdc_during_playback_frames"] = int(info["chain_pdc_frames"])
        result["plugin_pdc_during_playback_frames"] = int(info["plugin_pdc_during_playback"])
    except (ValueError, KeyError):
        pass
    if source == "virtual" and instrumented and float(info.get("first_audio_wall", 0)) > 0:
        result["note_on_to_track_processing_ms"] = 1000 * (float(info["first_audio_wall"]) - float(info["note_on"]))
    if instrumented:
        cadence = np.genfromtxt(run / "cadence.tsv", names=True, delimiter="\t")
        intervals = np.diff(np.atleast_1d(cadence["wall_seconds"]))
        track = np.fromfile(run / "track.f32", dtype="<f4").reshape(-1, 2)
        sf.write(run / "track.wav", track, 48000, subtype="FLOAT")
        result.update({"track_frames": len(track), "track_peak": float(np.max(np.abs(track), initial=0)),
                       "callback_count": len(np.atleast_1d(cadence)),
                       "callback_interval_median_ms": float(np.median(intervals) * 1000) if len(intervals) else None,
                       "callback_interval_max_ms": float(np.max(intervals) * 1000) if len(intervals) else None,
                       "callback_intervals_over_two_periods": int(np.sum(intervals > (2 * quantum / 48000)))})
    if jack_rate != 48000 or offline_rate != 48000 or jack.shape[1] != 2 or offline.shape[1] != 2:
        failures.append("audio capture format is not 48 kHz stereo")
    if not np.isfinite(jack).all() or not np.isfinite(offline).all() or (instrumented and not np.isfinite(track).all()):
        failures.append("live capture or reference contains NaN or infinity")

    def aligned(a: np.ndarray, b: np.ndarray) -> tuple[float, float]:
        a_on = np.flatnonzero(np.max(np.abs(a), axis=1) > 1e-5)
        b_on = np.flatnonzero(np.max(np.abs(b), axis=1) > 1e-5)
        if not len(a_on) or not len(b_on):
            raise ValueError("one audio capture is silent")
        count = min(len(a) - int(a_on[0]), len(b) - int(b_on[0]), 48000)
        if count < 47000:
            raise ValueError("audio capture is too short")
        left = a[int(a_on[0]):int(a_on[0]) + count].reshape(-1)
        right = b[int(b_on[0]):int(b_on[0]) + count].reshape(-1)
        energy = float(np.sqrt(np.mean(np.square(left))))
        if energy < 1e-6:
            raise ValueError("audio capture has insufficient energy")
        return float(np.corrcoef(left, right)[0, 1]), float(np.sqrt(np.mean(np.square(left - right))) / energy)

    try:
        result["jack_vs_reference_correlation"], result["jack_vs_reference_relative_rms_error"] = aligned(offline, jack)
        if (not math.isfinite(result["jack_vs_reference_correlation"]) or
                not math.isfinite(result["jack_vs_reference_relative_rms_error"]) or
                result["jack_vs_reference_correlation"] < 0.98 or result["jack_vs_reference_relative_rms_error"] > 0.15):
            failures.append("JACK output does not match the reference")
        if instrumented:
            result["track_vs_reference_correlation"], result["track_vs_reference_relative_rms_error"] = aligned(offline, track)
            result["jack_vs_track_correlation"], result["jack_vs_track_relative_rms_error"] = aligned(track, jack)
            if (not math.isfinite(result["track_vs_reference_correlation"]) or
                    not math.isfinite(result["track_vs_reference_relative_rms_error"]) or
                    result["track_vs_reference_correlation"] < 0.98 or result["track_vs_reference_relative_rms_error"] > 0.15):
                failures.append("live track output does not match the reference")
            if (not math.isfinite(result["jack_vs_track_correlation"]) or
                    not math.isfinite(result["jack_vs_track_relative_rms_error"]) or
                    result["jack_vs_track_correlation"] < 0.98 or result["jack_vs_track_relative_rms_error"] > 0.15):
                failures.append("JACK output does not match the captured track output")
    except (ValueError, IndexError) as exc:
        failures.append(str(exc))

    samples = {"reaper": [], "recorder": []}
    for line in (run / "pw-top.log").read_text().splitlines():
        try:
            timestamp, snapshot = line.split("\t", 1)
            columns = snapshot.split()
            if len(columns) >= 10:
                if columns[-1].upper().startswith("REAPER"):
                    samples["reaper"].append((float(timestamp), int(columns[8])))
                elif columns[-1] == capture_name:
                    samples["recorder"].append((float(timestamp), int(columns[8])))
        except (ValueError, IndexError):
            continue
    for name, entries in samples.items():
        during = [entry for entry in entries if started <= entry[0] <= stopped]
        earlier = [entry for entry in entries if entry[0] < started]
        if not earlier:
            failures.append(f"PipeWire did not report the {name} error counter before playback")
        elif len(during) < 2:
            failures.append(f"PipeWire did not report enough {name} error samples during playback")
        else:
            first = earlier[-1][1]
            last = during[-1][1]
            result[f"pipewire_{name}_error_counter_start"] = first
            result[f"pipewire_{name}_error_counter_end"] = last
            result[f"pipewire_{name}_error_samples_during_playback"] = len(during)
            if last > first:
                failures.append(f"PipeWire {name} error counter increased by {last - first}")
    cpu_rows = []
    for line in (run / "system-cpu.tsv").read_text().splitlines()[1:]:
        timestamp, aggregate, max_core = map(float, line.split("\t"))
        cpu_rows.append((timestamp, aggregate, max_core))
    node_started = float((run / "node-started").read_text())
    before = [row for row in cpu_rows if row[0] < node_started]
    playback = [row for row in cpu_rows if started <= row[0] <= stopped]
    if before:
        result["cpu_before_node_max_aggregate_pct"] = max(row[1] for row in before)
        result["cpu_before_node_max_core_pct"] = max(row[2] for row in before)
    if playback:
        result["cpu_playback_max_aggregate_pct"] = max(row[1] for row in playback)
        result["cpu_playback_max_core_pct"] = max(row[2] for row in playback)
    result["capture_node"] = capture_name
    result["status"] = "Failed" if failures else "Passed"
    result["failures"] = failures
    result = {key: (None if isinstance(value, float) and not math.isfinite(value) else value)
              for key, value in result.items()}
    payload = json.dumps(result, indent=2, allow_nan=False)
    (run / "live-analysis.json").write_text(payload + "\n")
    print(payload, flush=True)
    return int(bool(failures))


def open_live(source_run: Path, case_name: str, source: str, anticipative: bool, timeout: float,
              quantum: int, instrumented: bool) -> int:
    source_run = source_run.expanduser().resolve(strict=True)
    if not source_run.is_relative_to((VERIFY / "reaper-runs").resolve()):
        raise ValueError("live project must originate in a private verification run")
    identity = json.loads((source_run / "identity.json").read_text())
    binary = Path(identity["installed_binary"]).resolve(strict=True)
    if hashlib.sha256(binary.read_bytes()).hexdigest() != identity["sha256_before"]:
        raise ValueError("installed binary changed since the reference run")
    reference_project = (source_run / "projects" / (case_name + ".RPP")).resolve(strict=True)
    if not reference_project.is_relative_to(source_run / "projects"):
        raise ValueError("live project escaped its evidence directory")
    run_id = "live-" + time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:8]
    run = VERIFY / "reaper-runs" / run_id
    profile = VERIFY / "reaper-resource" / run_id
    for folder in (run / "jobs", run / "projects", run / "empty-plugins", profile / "vst3", profile / "Effects"):
        folder.mkdir(parents=True, exist_ok=False)
    bundle = binary.parent.parent.parent
    (profile / "vst3/SliceLab.vst3").symlink_to(bundle, target_is_directory=True)
    (profile / "reaper.ini").write_text("[reaper]\nvstpath=" + str(profile / "vst3") + "\n")
    registration = Path.home() / ".config/REAPER/reaper-reginfo2.ini"
    if registration.is_file():
        shutil.copy2(registration, profile / registration.name)
    project = run / "projects/live.RPP"
    shutil.copy2(reference_project, project)
    gmem = "SliceLab_" + uuid.uuid4().hex
    if instrumented:
        (profile / "Effects/SliceLabCapture").write_text(live_capture_effect(gmem))
    capture_name = "slicelab_capture_" + uuid.uuid4().hex[:8]
    def ports(direction: str) -> set[str]:
        result = subprocess.run(["pw-link", direction], check=True, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
        return set(result.stdout.splitlines())
    existing_outputs = ports("-o")
    job = {"resource": str(profile), "project": str(project), "mode": "live", "name": "capture",
           "result": str(run / "jobs/live-capture.result"),
           "parameters": str(run / "jobs/live-capture.parameters.tsv"),
           "metadata": str(run / "jobs/live-capture.metadata.tsv"), "plugin_binary": str(binary),
           "ready": str(run / "jobs/live.ready"), "go": str(run / "jobs/live.go"),
           "capture": str(run / "track.f32"), "cadence": str(run / "cadence.tsv"),
           "live_info": str(run / "live-info.tsv"), "gmem": gmem, "pipewire": True,
           "live_source": source, "anticipative": anticipative, "quantum": quantum,
           "instrumented": instrumented, "reference_audio": str(VERIFY / "fixtures/tone.wav")}
    (run / "identity.json").write_text(json.dumps({"installed_binary": str(binary),
        "sha256_before": identity["sha256_before"], "reference_run": str(source_run),
        "reference_case": case_name, "live_source": source, "anticipative": anticipative,
        "quantum": quantum, "instrumented": instrumented,
        "run_id": run_id}, indent=2) + "\n")
    print(f"Live REAPER evidence directory: {run}", flush=True)
    cpu_stop = threading.Event()
    def read_cpu() -> dict[str, tuple[int, int]]:
        result = {}
        for line in Path("/proc/stat").read_text().splitlines():
            fields = line.split()
            if fields and (fields[0] == "cpu" or fields[0][3:].isdigit() and fields[0].startswith("cpu")):
                values = list(map(int, fields[1:9]))
                result[fields[0]] = (sum(values), values[3] + values[4])
        return result

    def record_cpu() -> None:
        previous = read_cpu()
        with (run / "system-cpu.tsv").open("w") as log:
            log.write("time\taggregate_pct\tmax_core_pct\n")
            while not cpu_stop.wait(0.2):
                current = read_cpu()
                busy = {}
                for name, (total, idle) in current.items():
                    if name in previous:
                        elapsed = total - previous[name][0]
                        if elapsed:
                            busy[name] = 100 * (1 - (idle - previous[name][1]) / elapsed)
                if "cpu" in busy and len(busy) > 1:
                    log.write(f'{time.monotonic():.9f}\t{busy["cpu"]:.3f}\t{max(v for k, v in busy.items() if k != "cpu"):.3f}\n')
                    log.flush()
                previous = current

    cpu_thread = threading.Thread(target=record_cpu, daemon=True)
    cpu_thread.start()
    time.sleep(1.0)
    (run / "node-started").write_text(str(time.monotonic()))
    with (run / "pw-record.log").open("wb") as capture_log:
        recorder = subprocess.Popen(["pw-record", "--target", "0", "--rate", "48000", "--channels", "2",
            "--format", "f32", "--properties", "{ node.name = " + capture_name + " }", str(run / "jack.wav")],
            stdout=capture_log, stderr=subprocess.STDOUT)
        monitor = subprocess.Popen(["stdbuf", "-oL", "pw-top", "-b", "-n", "20"], stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True, bufsize=1)

        def record_top() -> None:
            with (run / "pw-top.log").open("w") as log:
                for line in monitor.stdout:
                    log.write(f"{time.monotonic():.9f}\t{line}")
                    log.flush()

        monitor_thread = threading.Thread(target=record_top, daemon=True)
        monitor_thread.start()

        def connect(process: subprocess.Popen) -> None:
            deadline = time.monotonic() + 8
            targets = [capture_name + ":input_FL", capture_name + ":input_FR"]
            sources = None
            while time.monotonic() < deadline:
                if process.poll() is not None or recorder.poll() is not None:
                    raise RuntimeError("REAPER or the private audio recorder exited")
                candidates = ports("-o") - existing_outputs
                left = [name for name in candidates if "REAPER" in name.upper() and name.endswith(":out1")]
                right = [name for name in candidates if "REAPER" in name.upper() and name.endswith(":out2")]
                if len(left) == 1 and len(right) == 1 and set(targets).issubset(ports("-i")):
                    sources = [left[0], right[0]]
                    break
                time.sleep(0.05)
            if sources is None:
                raise RuntimeError("could not identify unique REAPER and private capture ports")
            for out_port, in_port in zip(sources, targets):
                subprocess.run(["pw-link", out_port, in_port], check=True, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, timeout=5)
            (run / "ports.json").write_text(json.dumps({"source": sources, "capture": targets}, indent=2) + "\n")
            # Let the new PipeWire links enter the graph before sending live MIDI or
            # starting the transport; the recorder must already be running.
            time.sleep(1.5)
            Path(job["go"]).write_text("GO\n")

        try:
            start_job(job, profile, run, timeout, on_ready=connect)
        finally:
            end_owned_process(recorder)
            end_owned_process(monitor)
            monitor_thread.join(timeout=3)
            cpu_stop.set()
            cpu_thread.join(timeout=3)
    if hashlib.sha256(binary.read_bytes()).hexdigest() != identity["sha256_before"]:
        raise RuntimeError("installed binary changed during live capture")
    reference = VERIFY / "fixtures/tone.wav" if source == "graph" else source_run / "renders" / (case_name + ".wav")
    return evaluate_live(run, reference, capture_name, quantum, instrumented, source)


def blank_project(path: Path) -> None:
    path.write_text('<REAPER_PROJECT 0.1 "7.0/x64" 0.0\n>\n')


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bundle", type=Path, default=Path.home() / ".vst3/SliceLab.vst3")
    parser.add_argument("--cases", type=Path, default=VERIFY / "cases.json")
    parser.add_argument("--only", nargs="*")
    parser.add_argument("--timeout", type=float, default=50)
    parser.add_argument("--gui-run", type=Path)
    parser.add_argument("--gui-source", type=Path)
    parser.add_argument("--gui-snapshot", type=Path)
    parser.add_argument("--gui-save", action="store_true")
    parser.add_argument("--gui-window", type=lambda value: int(value, 0))
    parser.add_argument("--gui-capture", action="store_true")
    parser.add_argument("--gui-record", action="store_true")
    parser.add_argument("--record-seconds", type=float, default=8)
    parser.add_argument("--pipewire", action="store_true")
    parser.add_argument("--live-run", type=Path)
    parser.add_argument("--live-case", default="pitch_octave")
    parser.add_argument("--live-source", choices=("timeline", "virtual", "graph"), default="timeline")
    parser.add_argument("--disable-anticipative", action="store_true")
    parser.add_argument("--quantum", type=int, choices=(128, 512), default=512)
    parser.add_argument("--instrumented", action="store_true")
    parser.add_argument("--cold-delay", type=float, default=0)
    parser.add_argument("--cold-immediate", action="store_true")
    parser.add_argument("--inspect-tempo-map", action="store_true")
    args = parser.parse_args()
    if args.gui_snapshot:
        if args.gui_record:
            return record_gui(args.gui_snapshot, args.record_seconds)
        if args.gui_capture:
            if args.gui_window is None:
                raise ValueError("--gui-capture requires --gui-window")
            return capture_gui(args.gui_snapshot, args.gui_window)
        return snapshot_gui(args.gui_snapshot, args.timeout, args.gui_save)
    if args.gui_run:
        return open_gui(args.gui_run, args.timeout, args.pipewire, args.quantum, args.gui_source)
    if args.live_run:
        return open_live(args.live_run, args.live_case, args.live_source, not args.disable_anticipative,
                         args.timeout, args.quantum, args.instrumented)
    bundle = args.bundle.expanduser().resolve(strict=True)
    if not bundle.is_dir() or bundle.suffix != ".vst3":
        raise ValueError("--bundle must name an installed VST3 bundle")
    binaries = list((bundle / "Contents/x86_64-linux").glob("*.so"))
    if len(binaries) != 1:
        raise ValueError("expected one Linux shared library in the VST3 bundle")
    binary = binaries[0]
    cases = json.loads(args.cases.read_text())
    for case in cases:
        if case["name"] == "neutral":
            case["expected"]["timing_correlation"] = True
            case["expected"]["exact_source_extent"] = True
        if case["name"] == "sample_original":
            case["expected"]["original_source_match"] = True
        for stem in ("vocals", "drums", "bass", "other"):
            if case["name"] == "sample_" + stem:
                case["expected"]["stem_source_match"] = stem
    if any(case["name"] == "neutral" for case in cases):
        neutral = next(case for case in cases if case["name"] == "neutral")
        auto = dict(neutral, name="host_gain_automation", automation_parameter="Master gain",
                    expected={"automation_gain": True})
        cases.append(auto)
        cases.append(dict(neutral, name="aux_output", channels=4,
                          state_overrides={"pad01_output": 1},
                          expected={"aux_output": True, "frequency_hz": 440}))
        cases.append(dict(neutral, name="last_bus_output", channels=64,
                          state_overrides={"pad01_output": 31}, expected={"last_aux_output": True}))
        frames = [index * 5120 + offset for index, offset in enumerate((17, 63, 64, 127, 128, 129))]
        notes = [{"note": 36, "start": frame / 48000, "end": (frame + 240) / 48000,
                  "velocity": 127} for frame in frames]
        cases.append(dict(neutral, name="midi_offsets", length=0.65, pad_end=0.05,
                          midi_ppq=96000, notes=notes, expected={"midi_frames": frames}))
        bounds = [[index / 8, (index + 1) / 8] for index in range(32)]
        pad_frames = [960 + index * 7680 for index in range(32)]
        pad_notes = [{"note": 36 + index, "start": frame / 48000, "end": (frame + 2400) / 48000,
                      "velocity": 127} for index, frame in enumerate(pad_frames)]
        pad_changes = {}
        for index in range(1, 33):
            for key, value in {"attack": 0, "release": 0, "gain": 0, "pan": 0,
                               "semitones": 0, "cents": 0, "time": 1,
                               "reverse": 0, "loop": 0, "output": 0}.items():
                pad_changes[f"pad{index:02d}_{key}"] = value
        cases.append(dict(neutral, name="all_pad_mapping", length=5.2, midi_ppq=96000,
                          source_override=str(VERIFY / "fixtures/coded.wav"), pad_bounds=bounds,
                          state_overrides=pad_changes, notes=pad_notes,
                          expected={"pad_frames": pad_frames, "pad_bounds": bounds}))
        chord_notes = [{"note": note, "start": 0.02, "end": 0.07, "velocity": 127} for note in (36, 37)]
        for mode, mono in (("poly_chord", 0), ("mono_chord", 1)):
            cases.append(dict(neutral, name=mode, length=0.3, midi_ppq=96000,
                              source_override=str(VERIFY / "fixtures/coded.wav"), pad_bounds=bounds,
                              state_overrides=dict(pad_changes, mono=mono), notes=chord_notes,
                              expected={"chord_notes": [36, 37], "chord_poly": not mono}))
        burst_notes = [{"note": 36 + index, "start": 0.02, "end": 0.07, "velocity": 127}
                       for index in range(32)]
        cases.append(dict(neutral, name="poly_32_stretched", length=0.3, midi_ppq=96000,
                          source_override=str(VERIFY / "fixtures/coded.wav"), pad_bounds=bounds,
                          state_overrides=dict(pad_changes, mono=0, global_semitones=12), notes=burst_notes,
                          expected={"burst_frequencies": [(220 + index * 30) * 2 for index in range(32)]}))
        velocity_notes = [{"note": 36, "start": 0.02, "end": 0.04, "velocity": 127},
                          {"note": 36, "start": 0.12, "end": 0.14, "velocity": 32}]
        for name, sensitive in (("velocity_sensitive", 1), ("velocity_fixed", 0)):
            cases.append(dict(neutral, name=name, length=0.25, pad_end=0.05, notes=velocity_notes,
                              state_overrides={"velocity_sensitive": sensitive},
                              expected={"velocity_sensitive": bool(sensitive)}))
        for name, changes, target in (
            ("pitch_down_octave", {"global_semitones": -12}, 220.0),
            ("pitch_up_fifth", {"global_semitones": 7}, 440 * 2 ** (7 / 12)),
            ("pitch_up_50_cents", {"global_cents": 50}, 440 * 2 ** (0.5 / 12)),
            ("pitch_down_50_cents", {"global_cents": -50}, 440 * 2 ** (-0.5 / 12)),
        ):
            cases.append(dict(neutral, name=name, state_overrides=changes,
                              expected={"frequency_hz": target, "duration": 1.0}))
        for name, note in (("keyboard_root", 60), ("keyboard_fifth", 67), ("keyboard_octave", 72)):
            keyboard_notes = [dict(neutral["notes"][0], note=note)]
            cases.append(dict(neutral, name=name, state_overrides={"keyboard": 1, "root_note": 60},
                              notes=keyboard_notes,
                              expected={"frequency_hz": 440 * 2 ** ((note - 60) / 12), "duration": 1.0}))
        cases.append(dict(neutral, name="repitch_octave", state_overrides={"repitch": 1, "global_semitones": 12},
                          expected={"frequency_hz": 880, "duration": 0.5, "frequency_window": 0.30,
                                    "frequency_margin_start": 0.05}))
        cases.append(dict(neutral, name="repitch_half_tempo", length=2.3,
                          state_overrides={"repitch": 1, "target_bpm": 60},
                          expected={"frequency_hz": 220, "duration": 2.0}))
        cases.append(dict(neutral, name="host_tempo_transition", length=7.4, pad_end=4.0,
                          source_override=str(VERIFY / "fixtures/click.wav"),
                          state_overrides={"sync": 1},
                          tempo_markers=[{"time": 0.0, "bpm": 120}, {"time": 1.0, "bpm": 60}],
                          expected={"tempo_pulses": [0.02, 0.52, 1.04, 2.04, 3.04, 4.04, 5.04, 6.04],
                                    "empty_pulse_midpoints": [1.54, 2.54, 3.54, 4.54, 5.54]}))
        reverse = next((case for case in cases if case["name"] == "reverse"), None)
        if reverse:
            reverse_notes = [dict(reverse["notes"][0], end=0.82)]
            cases.append(dict(reverse, name="reverse_hold", length=1.2,
                              state_overrides={"hold": 1, "pad01_loop": 1}, notes=reverse_notes,
                              expected={"first_half_louder": True, "stop_before": 0.9}))
        sample = next((case for case in cases if case["name"] == "sample_vocals"), None)
        if sample:
            cases.append(dict(sample, name="host_stem_automation", automation_parameter="vocals gain",
                              expected={"audible": True}))
            original = next(case for case in cases if case["name"] == "sample_original")
            cases.append(dict(original, name="sample_solo_vocals", state_overrides={"stem_vocals_solo": 1},
                              expected={"stem_source_match": "vocals"}))
    if args.only:
        requested = set(args.only)
        available = {case["name"] for case in cases}
        missing = requested - available
        if missing:
            raise ValueError("unknown case names: " + ", ".join(sorted(missing)))
        if requested & {"sample_vocals", "sample_drums", "sample_bass", "sample_other", "host_stem_automation", "sample_solo_vocals"}:
            requested.add("sample_original")
        if requested & {"host_stem_automation", "sample_solo_vocals"}:
            requested.add("sample_vocals")
        cases = [case for case in cases if case["name"] in requested]

    run_id = time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:8]
    run = VERIFY / "reaper-runs" / run_id
    profile = VERIFY / "reaper-resource" / run_id
    for folder in (run / "jobs", run / "projects", run / "renders", run / "chunks",
                   run / "empty-plugins", profile / "vst3"):
        folder.mkdir(parents=True, exist_ok=False)
    (profile / "vst3/SliceLab.vst3").symlink_to(bundle, target_is_directory=True)
    (profile / "reaper.ini").write_text("[reaper]\nvstpath=" + str(profile / "vst3") + "\n")
    registration = Path.home() / ".config/REAPER/reaper-reginfo2.ini"
    if registration.is_file():
        shutil.copy2(registration, profile / registration.name)
    binary_hash = hashlib.sha256(binary.read_bytes()).hexdigest()
    source_revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True,
                                     stdout=subprocess.PIPE, check=True).stdout.strip()
    (run / "identity.json").write_text(json.dumps({"installed_binary": str(binary),
        "sha256_before": binary_hash, "git_revision": source_revision,
        "fixture_source": str(args.cases.resolve()), "render_rate": 48000,
        "reaper_executable": str(REAPER), "run_id": run_id}, indent=2) + "\n")
    print(f"REAPER evidence directory: {run}", flush=True)

    def job_for(mode: str, case: dict) -> dict:
        name = case["name"]
        project = run / "projects" / (("probe" if mode == "probe" else case.get("project_name", name)) + ".RPP")
        if mode != "cold":
            blank_project(project)
        output = run / "jobs" / (mode + "-" + name)
        return {"resource": str(profile), "project": str(project), "mode": mode, "name": name,
                "result": str(output) + ".result", "parameters": str(output) + ".parameters.tsv",
                "metadata": str(output) + ".metadata.tsv", "chunk": str(run / "chunks" / (name + ".b64")),
                "plugin_binary": str(binary.resolve()),
                "actual_chunk": str(run / "chunks" / (name + ".actual.b64")),
                "render_dir": str(run / "renders"), "render_format": base64.b64encode(b"evaw\x20\0\0").decode(),
                "length": case.get("length", 1), "bpm": case.get("bpm", 120), "channels": case.get("channels", 2),
                "midi_ppq": case.get("midi_ppq", 0),
                "tempo_markers": case.get("tempo_markers", []), "tempo_report": str(output) + ".tempo.tsv",
                "cold_delay": args.cold_delay if mode == "cold" else 0,
                "cold_immediate": args.cold_immediate if mode == "cold" else False,
                "cold_ready": str(output) + ".ready.tsv",
                "inspect_tempo_map": args.inspect_tempo_map and mode == "cold",
                "notes": [dict(note, finish=note["end"]) for note in case.get("notes", [])],
                "automation_parameter": case.get("automation_parameter", "")}

    probe = job_for("probe", {"name": "probe"})
    start_job(probe, profile, run, args.timeout)
    template = base64.b64decode(Path(probe["chunk"]).read_bytes(), validate=True)
    xml_state(template)
    execution = []
    spec = importlib.util.spec_from_file_location("slicelab_render_checks", ROOT / "scripts/check-reaper-renders.py")
    checker = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(checker)
    incremental = []

    def inspect_render(case: dict) -> None:
        try:
            result = checker.check_case(case, run / "renders" / (case["name"] + ".wav"))
        except (OSError, RuntimeError, ValueError) as exc:
            result = {"name": case["name"], "status": "Failed", "failures": [str(exc)]}
        incremental.append(result)
        (run / "audio-incremental.json").write_text(json.dumps(incremental, indent=2) + "\n")
        if result["status"] != "Passed":
            print("AUDIO FAILED " + case["name"] + ": " + "; ".join(result["failures"][:3]), flush=True)

    for case in cases:
        print("Rendering " + case["name"], flush=True)
        job = job_for("case", case)
        fixture = Path(case["state"]).read_bytes()
        if case.get("state_overrides") or "pad_end" in case or case.get("pad_bounds") or case.get("source_override"):
            fixture = state_with_overrides(fixture, case.get("state_overrides", {}), case.get("pad_end"),
                                           case.get("pad_bounds"), Path(case["source_override"]) if case.get("source_override") else None)
            modified_state = run / "chunks" / (case["name"] + ".slicelab")
            modified_state.write_bytes(fixture)
            case["state"] = str(modified_state)
        chunk = replace_component(template, fixture)
        Path(job["chunk"]).write_bytes(base64.b64encode(chunk))
        try:
            start_job(job, profile, run, args.timeout)
            actual = base64.b64decode(Path(job["actual_chunk"]).read_bytes(), validate=True)
            check_restored(fixture, actual)
            execution.append({"name": case["name"], "status": "Passed"})
            inspect_render(case)
        except Exception as exc:
            execution.append({"name": case["name"], "status": "Failed", "error": str(exc)})
            print("FAILED " + case["name"] + ": " + str(exc).splitlines()[0], flush=True)
    for original_name in ("neutral", "host_gain_automation", "host_tempo_transition", "sample_original"):
        source_case = next((case for case in cases if case["name"] == original_name), None)
        if not source_case:
            continue
        cold = dict(source_case, name="cold_" + original_name, project_name=original_name)
        print("Rendering " + cold["name"] + " in a fresh process", flush=True)
        try:
            start_job(job_for("cold", cold), profile, run, args.timeout)
            cases.append(cold)
            execution.append({"name": cold["name"], "status": "Passed"})
            inspect_render(cold)
        except Exception as exc:
            execution.append({"name": cold["name"], "status": "Failed", "error": str(exc)})
            print("FAILED " + cold["name"] + ": " + str(exc).splitlines()[0], flush=True)
    (run / "cases.json").write_text(json.dumps(cases, indent=2) + "\n")
    (run / "execution.json").write_text(json.dumps(execution, indent=2) + "\n")
    binary_hash_after = hashlib.sha256(binary.read_bytes()).hexdigest()
    (run / "binary-sha256-after.txt").write_text(binary_hash_after + "\n")
    result = subprocess.run([sys.executable, str(ROOT / "scripts/check-reaper-renders.py"),
        str(run / "cases.json"), str(run / "renders"), "--output", str(run / "audio-checks.json")], check=False)
    if binary_hash_after != binary_hash:
        print("FAILED installed plugin changed during the verification run", flush=True)
    return int(result.returncode != 0 or binary_hash_after != binary_hash or
               any(case["status"] != "Passed" for case in execution))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print("REAPER verification failed: " + str(exc), file=sys.stderr)
        sys.exit(1)
