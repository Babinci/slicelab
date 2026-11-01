#!/usr/bin/env python3
"""Check rendered REAPER audio against independent fixture expectations."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import sys
import xml.etree.ElementTree as ET

import numpy as np
import soundfile as sf
from scipy.signal import periodogram, resample_poly


def rms(samples: np.ndarray) -> float:
    return float(np.sqrt(np.mean(np.square(samples)))) if samples.size else 0.0


def finite_json(value):
    if isinstance(value, float) and not math.isfinite(value):
        return None
    if isinstance(value, dict):
        return {key: finite_json(item) for key, item in value.items()}
    if isinstance(value, list):
        return [finite_json(item) for item in value]
    return value


def inspect_midi(path: Path) -> dict:
    data = path.read_bytes()
    if len(data) < 14 or data[:4] != b"MThd":
        raise ValueError("export is not a Standard MIDI File")
    header_size = struct.unpack_from(">I", data, 4)[0]
    if header_size < 6 or 8 + header_size > len(data):
        raise ValueError("MIDI header is truncated")
    format_type, track_count, division = struct.unpack_from(">HHH", data, 8)
    if format_type > 2 or track_count == 0 or division == 0:
        raise ValueError("MIDI header contains an invalid format, track count, or timebase")
    position = 8 + header_size
    notes = []
    note_offs = 0
    active_notes = {}
    matched_notes = 0

    def variable_length(buffer: bytes, offset: int) -> tuple[int, int]:
        value = 0
        for _ in range(4):
            if offset >= len(buffer):
                raise ValueError("truncated MIDI variable-length integer")
            byte = buffer[offset]
            offset += 1
            value = (value << 7) | (byte & 0x7f)
            if byte < 128:
                return value, offset
        raise ValueError("MIDI variable-length integer is too long")

    for _ in range(track_count):
        if position + 8 > len(data) or data[position:position + 4] != b"MTrk":
            raise ValueError("MIDI track is missing")
        size = struct.unpack_from(">I", data, position + 4)[0]
        position += 8
        if position + size > len(data):
            raise ValueError("MIDI track is truncated")
        track = data[position:position + size]
        position += size
        at, tick, running = 0, 0, None
        while at < len(track):
            delta, at = variable_length(track, at)
            tick += delta
            if at >= len(track):
                raise ValueError("MIDI event is truncated")
            if track[at] >= 128:
                status = track[at]
                at += 1
                if status < 0xf0:
                    running = status
            elif running is not None:
                status = running
            else:
                raise ValueError("MIDI event has no running status")
            if status == 0xff:
                if at >= len(track):
                    raise ValueError("MIDI metadata is truncated")
                at += 1
                length, at = variable_length(track, at)
                at += length
                running = None
            elif status in (0xf0, 0xf7):
                length, at = variable_length(track, at)
                at += length
                running = None
            elif 0x80 <= status < 0xf0:
                kind = status & 0xf0
                length = 1 if kind in (0xc0, 0xd0) else 2
                if at + length > len(track) or any(value >= 128 for value in track[at:at + length]):
                    raise ValueError("MIDI channel event is truncated or invalid")
                note = track[at]
                velocity = track[at + 1] if length == 2 else 0
                if kind == 0x90 and velocity:
                    notes.append({"tick": tick, "channel": (status & 15) + 1, "note": note, "velocity": velocity})
                    key = (status & 15, note)
                    active_notes[key] = active_notes.get(key, 0) + 1
                elif kind == 0x80 or (kind == 0x90 and not velocity):
                    note_offs += 1
                    key = (status & 15, note)
                    if active_notes.get(key, 0) > 0:
                        active_notes[key] -= 1
                        matched_notes += 1
                at += length
            else:
                raise ValueError("unsupported MIDI event status")
            if at > len(track):
                raise ValueError("MIDI event data is truncated")
    if not notes or not note_offs or not matched_notes:
        raise ValueError("MIDI export does not contain a matched note-on and note-off")
    return {"format": format_type, "tracks": track_count, "division": division,
            "note_on_count": len(notes), "note_off_count": note_offs,
            "matched_note_count": matched_notes, "notes": notes}


def midi_note_events(case: dict, folder: Path, rate: int) -> list[tuple[int, int]]:
    project = folder.parent / "projects" / (case.get("project_name", case["name"]) + ".RPP")
    contents = project.read_text()
    ppq = re.search(r"^\s*HASDATA\s+1\s+(\d+)\s+QN", contents, re.MULTILINE)
    events = re.findall(r"^\s*E\s+(\d+)\s+([0-9a-f]{2})\s+([0-9a-f]{2})\s+([0-9a-f]{2})",
                        contents, re.MULTILINE | re.IGNORECASE)
    if not ppq or not events:
        raise ValueError("could not find the expected saved MIDI note")
    position = 0
    notes = []
    for delta, status, note, velocity in events:
        position += int(delta)
        if int(status, 16) & 0xf0 == 0x90 and int(velocity, 16) > 0:
            notes.append((int(note, 16), round(position / int(ppq.group(1)) * 60 / case["bpm"] * rate)))
    if not notes:
        raise ValueError("could not find the expected saved MIDI note")
    return notes


def midi_event_frames(case: dict, folder: Path, rate: int) -> list[int]:
    return [frame for note, frame in midi_note_events(case, folder, rate) if note == 36]


def original_source(case: dict, stem: str | None = None) -> tuple[np.ndarray, int]:
    data = Path(case["state"]).read_bytes()
    if data[:4] != b"VC2!" or len(data) < 9:
        raise ValueError("invalid fixture state")
    length = struct.unpack_from("<I", data, 4)[0]
    root = ET.fromstring(data[8:8 + length])
    if stem:
        manifest_path = root.get("stemManifest")
        if not manifest_path:
            raise ValueError("fixture has no stem manifest")
        source = json.loads(Path(manifest_path).read_text())["stems"][stem]
    else:
        source = root.get("source")
    if not source:
        raise ValueError("fixture source is absent")
    with sf.SoundFile(source) as file:
        rate = file.samplerate
        return file.read(frames=4 * rate, dtype="float64", always_2d=True), rate


def check_case(case: dict, path: Path) -> dict:
    failures: list[str] = []
    audio, rate = sf.read(path, dtype="float64", always_2d=True)
    bad_samples = int(audio.size - np.count_nonzero(np.isfinite(audio)))
    if bad_samples:
        failures.append("render contains NaN or infinity")
        audio = np.nan_to_num(audio, nan=0.0, posinf=0.0, neginf=0.0)
    expect = case["expected"]
    measured = {"sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "sample_rate": rate, "channels": audio.shape[1], "subtype": sf.info(path).subtype,
                "file_duration": len(audio) / rate, "peak": float(np.max(np.abs(audio), initial=0)),
                "rms": rms(audio)}
    measured["nonfinite_samples"] = bad_samples
    if rate != 48000:
        failures.append(f"sample rate {rate} is not 48000 Hz")
    if measured["subtype"] != "FLOAT":
        failures.append(f"WAV subtype {measured['subtype']} is not 32-bit float")
    if audio.shape[1] != case.get("channels", 2):
        failures.append(f"render has {audio.shape[1]} channels, expected {case.get('channels', 2)}")
    if abs(measured["file_duration"] - case["length"]) > 1 / rate:
        failures.append(f"file duration {measured['file_duration']:.6f} differs from render bounds")
    if expect.get("silent"):
        if measured["peak"] > 1e-7:
            failures.append(f"expected silence, peak is {measured['peak']:.8g}")
    else:
        if measured["peak"] < 1e-4:
            failures.append("expected audible output, render is silent")

    width = max(1, rate // 100)
    count = len(audio) // width
    windows = audio[:count * width].reshape(count, width, audio.shape[1])
    window_rms = np.sqrt(np.mean(np.square(windows), axis=(1, 2)))
    threshold = max(1e-5, float(window_rms.max(initial=0)) * 0.01)
    active = np.flatnonzero(window_rms > threshold)
    if active.size:
        measured["audible_start"] = float(active[0] * width / rate)
        measured["audible_end"] = float((active[-1] + 1) * width / rate)
        measured["audible_duration"] = measured["audible_end"] - measured["audible_start"]
    else:
        measured.update(audible_start=None, audible_end=None, audible_duration=0.0)

    if "duration" in expect:
        target = expect["duration"]
        if abs(measured["audible_duration"] - target) > max(0.08, target * 0.05):
            failures.append(f"audible duration {measured['audible_duration']:.3f} s, expected {target:.3f} s")
    if "frequency_hz" in expect and active.size:
        target = expect["frequency_hz"]
        regions = []
        window = expect.get("frequency_window", 0.40)
        start = measured["audible_start"] + expect.get("frequency_margin_start", 0.10)
        while start + window <= measured["audible_end"] - 0.05:
            mono = audio[int(start * rate):int((start + window) * rate)].mean(axis=1)
            frequency, power = periodogram(mono, fs=rate, window="hann", nfft=rate * 4)
            hz = float(frequency[np.argmax(power)])
            cents = 1200 * math.log2(hz / target) if hz > 0 else 1000000.0
            regions.append({"start_seconds": round(start, 3), "frequency_hz": hz,
                            "error_cents": cents})
            start += window
        measured["pitch_regions"] = regions
        if not regions:
            failures.append("not enough steady audio to measure pitch")
        else:
            measured["frequency_hz"] = regions[0]["frequency_hz"]
            worst = max(regions, key=lambda region: abs(region["error_cents"]))
            measured["worst_pitch_error_cents"] = worst["error_cents"]
            if abs(worst["error_cents"]) > 5:
                failures.append(f"dominant spectral peak differs by {worst['error_cents']:.2f} cents; limit is 5 cents")
    if "stop_before" in expect:
        cutoff = int(expect["stop_before"] * rate)
        measured["post_release_rms"] = rms(audio[cutoff:])
        if measured["post_release_rms"] > max(1e-5, measured["peak"] * 0.005):
            failures.append("hold release did not stop before the specified time")
    if "audible_after" in expect:
        start = int(expect["audible_after"] * rate)
        measured["late_rms"] = rms(audio[start:min(len(audio), start + rate // 10)])
        if measured["late_rms"] < max(1e-5, measured["peak"] * 0.02):
            failures.append("loop is not audible after the specified time")
    if expect.get("first_half_louder") and active.size:
        start = int(measured["audible_start"] * rate)
        first = rms(audio[start + int(0.10 * rate):start + int(0.40 * rate)])
        second = rms(audio[start + int(0.60 * rate):start + int(0.90 * rate)])
        measured.update(first_half_rms=first, second_half_rms=second)
        if first < second * 2:
            failures.append("reverse output does not begin with the louder source half")
    if expect.get("right_silent") and audio.shape[1] >= 2:
        measured.update(left_rms=rms(audio[:, 0]), right_rms=rms(audio[:, 1]))
        if measured["left_rms"] < 1e-4 or measured["right_rms"] > max(1e-6, measured["left_rms"] * 0.01):
            failures.append("left pan does not isolate the left channel")
    if expect.get("aux_output") and audio.shape[1] == 4:
        main, aux = rms(audio[:, :2]), rms(audio[:, 2:])
        measured.update(main_rms=main, auxiliary_rms=aux)
        if aux < 1e-4 or main > max(1e-6, aux * 0.01):
            failures.append("pad assigned to output 2 did not isolate channels 3 and 4")
    if expect.get("last_aux_output") and audio.shape[1] == 64:
        earlier, last = rms(audio[:, :62]), rms(audio[:, 62:64])
        measured.update(earlier_channels_rms=earlier, last_stereo_bus_rms=last)
        if last < 1e-4 or earlier > max(1e-6, last * 0.01):
            failures.append("pad assigned to the final output did not isolate channels 63 and 64")
    if expect.get("automation_gain"):
        before = rms(audio[int(0.10 * rate):int(0.40 * rate)])
        after = rms(audio[int(0.65 * rate):int(0.90 * rate)])
        measured.update(before_automation_rms=before, after_automation_rms=after)
        if before < 1e-4 or after > before * 0.3:
            failures.append("host gain envelope did not reduce the held output")
    if "velocity_sensitive" in expect:
        strong = rms(audio[round(0.03 * rate):round(0.055 * rate)])
        soft = rms(audio[round(0.13 * rate):round(0.155 * rate)])
        ratio = soft / strong if strong > 1e-8 else 0
        measured.update(strong_velocity_rms=strong, soft_velocity_rms=soft, velocity_ratio=ratio)
        low, high = (0.15, 0.40) if expect["velocity_sensitive"] else (0.95, 1.05)
        if strong < 1e-5 or not low <= ratio <= high:
            failures.append("MIDI velocity did not have the expected effect on output level")
    if expect.get("burst_frequencies"):
        try:
            events = midi_note_events(case, path.parent, rate)
        except (OSError, ValueError):
            events = []
        if len(events) != 32 or {note for note, _ in events} != set(range(36, 68)) or len({frame for _, frame in events}) != 1:
            failures.append("saved burst does not contain all 32 simultaneous MIDI notes")
        start, end = round(0.04 * rate), round(0.12 * rate)
        signal = audio[start:end].mean(axis=1)
        window = np.hanning(len(signal))
        spectrum = np.abs(np.fft.rfft(signal * window, n=rate * 2)) / (window.sum() / 2)
        frequencies = np.fft.rfftfreq(rate * 2, 1 / rate)
        burst = []
        for index, target in enumerate(expect["burst_frequencies"]):
            magnitude = float(spectrum[np.abs(frequencies - target) <= 8].max(initial=0))
            burst.append({"pad": index + 1, "target_hz": target, "peak_amplitude": magnitude})
            if magnitude < 0.03:
                failures.append(f"stretched 32-note burst lacks audible energy for pad {index + 1}")
        measured["burst_spectrum"] = burst
    if expect.get("tempo_pulses"):
        pulse_width = max(1, round(rate * 0.002))
        pulse_windows = audio[:len(audio) // pulse_width * pulse_width].reshape(-1, pulse_width, audio.shape[1])
        energy = np.sqrt(np.mean(np.square(pulse_windows), axis=(1, 2)))
        peak_energy = float(energy.max(initial=0))
        found = []
        for expected_time in expect["tempo_pulses"]:
            left = max(0, round((expected_time - 0.10) * rate / pulse_width))
            right = min(len(energy), round((expected_time + 0.10) * rate / pulse_width) + 1)
            if right <= left:
                raise ValueError("tempo pulse search is outside the render")
            index = left + int(np.argmax(energy[left:right]))
            actual_time = (index + 0.5) * pulse_width / rate
            error_ms = 1000 * (actual_time - expected_time)
            found.append({"expected_seconds": expected_time, "measured_seconds": actual_time,
                          "error_ms": error_ms, "rms": float(energy[index])})
            if abs(error_ms) > 80 or energy[index] < max(1e-5, peak_energy * 0.02):
                failures.append(f"host tempo pulse near {expected_time:.2f} s was missing or displaced by over 80 ms")
        extra = []
        for midpoint in expect.get("empty_pulse_midpoints", []):
            left = max(0, round((midpoint - 0.10) * rate / pulse_width))
            right = min(len(energy), round((midpoint + 0.10) * rate / pulse_width) + 1)
            value = float(energy[left:right].max(initial=0))
            extra.append({"seconds": midpoint, "rms": value})
            if value > max(1e-5, peak_energy * 0.10):
                failures.append(f"unexpected pulse near {midpoint:.2f} s after the host tempo change")
        measured.update(tempo_pulses=found, empty_pulse_midpoints=extra,
                        tempo_transient_tolerance_ms=80)
    if (expect.get("timing_correlation") or expect.get("exact_source_extent") or expect.get("original_source_match") or
            expect.get("stem_source_match") or expect.get("midi_frames") or expect.get("pad_frames") or
            expect.get("chord_notes")):
        try:
            onsets = midi_event_frames(case, path.parent, rate)
            onset = onsets[0]
            source, source_rate = original_source(case, expect.get("stem_source_match"))
            measured["saved_midi_event_frame"] = onset
            if source.shape[1] == 1:
                source = np.repeat(source, 2, axis=1)
            if expect.get("exact_source_extent"):
                if source_rate != rate:
                    raise ValueError("extent fixture has the wrong sample rate")
                source_active = np.flatnonzero(np.max(np.abs(source), axis=1) > 1e-5)
                output_active = np.flatnonzero(np.max(np.abs(audio), axis=1) > 1e-5)
                if not len(source_active) or not len(output_active):
                    raise ValueError("extent fixture or render is silent")
                expected_first = onset + int(source_active[0])
                expected_last = onset + int(source_active[-1])
                measured.update(source_extent_threshold=1e-5, source_expected_first_frame=expected_first,
                                source_expected_last_frame=expected_last,
                                render_first_frame=int(output_active[0]), render_last_frame=int(output_active[-1]))
                if abs(int(output_active[0]) - expected_first) > 1 or abs(int(output_active[-1]) - expected_last) > 1:
                    failures.append("neutral output extent differs from the source by more than one frame at the 1e-5 threshold")
            if expect.get("timing_correlation"):
                if source_rate != rate:
                    raise ValueError("timing fixture has the wrong sample rate")
                first, last = rate // 100, rate // 4
                reference = source[first:last, 0]
                correlations = [float(np.corrcoef(reference, audio[onset + first + shift:onset + last + shift, 0])[0, 1])
                                for shift in range(-8, 9)]
                best = int(np.argmax(correlations)) - 8
                measured["timing_correlation_offset_frames"] = best
                measured["timing_correlation"] = max(correlations)
                if abs(best) > 1 or max(correlations) < 0.99:
                    failures.append("neutral tone does not align with the saved MIDI event within one frame")
            if expect.get("midi_frames"):
                measured["saved_midi_event_frames"] = onsets
                if onsets != expect["midi_frames"]:
                    failures.append("saved MIDI events do not occupy the requested frames")
                if source_rate != rate:
                    raise ValueError("timing fixture has the wrong sample rate")
                first, last = rate // 100, rate // 25
                reference = source[first:last, 0]
                offsets = []
                for frame in onsets:
                    correlations = np.array([
                        np.corrcoef(reference, audio[frame + first + shift:frame + last + shift, 0])[0, 1]
                        for shift in range(-8, 9)])
                    correlations = np.nan_to_num(correlations, nan=-1)
                    best = int(np.argmax(correlations)) - 8
                    offsets.append({"event_frame": frame, "offset_frames": best,
                                    "correlation": float(np.max(correlations))})
                    if abs(best) > 1 or np.max(correlations) < 0.99:
                        failures.append(f"MIDI event at frame {frame} did not align within one frame")
                measured["midi_offsets"] = offsets
            if expect.get("pad_frames"):
                events = midi_note_events(case, path.parent, rate)
                expected_events = [(36 + index, frame) for index, frame in enumerate(expect["pad_frames"])]
                if events != expected_events:
                    failures.append("saved MIDI notes do not match the expected 32 pad mapping")
                if source_rate != rate or len(expect["pad_bounds"]) != 32:
                    raise ValueError("pad mapping fixture has the wrong rate or number of slices")
                first, last = round(0.015 * rate), round(0.10 * rate)
                comparisons = []
                for index, (_, frame) in enumerate(expected_events):
                    source_start = round(expect["pad_bounds"][index][0] * rate)
                    reference = source[source_start + first:source_start + last, 0]
                    correlations = np.array([
                        np.corrcoef(reference, audio[frame + first + shift:frame + last + shift, 0])[0, 1]
                        for shift in range(-8, 9)])
                    correlations = np.nan_to_num(correlations, nan=-1)
                    best = int(np.argmax(correlations)) - 8
                    comparisons.append({"pad": index + 1, "note": 36 + index,
                                        "event_frame": frame, "offset_frames": best,
                                        "correlation": float(np.max(correlations))})
                    if abs(best) > 1 or np.max(correlations) < 0.99:
                        failures.append(f"pad {index + 1} did not match its saved MIDI note and source interval")
                measured["pad_mapping"] = comparisons
            if expect.get("chord_notes"):
                events = midi_note_events(case, path.parent, rate)
                if len(events) != 2 or {note for note, _ in events} != set(expect["chord_notes"]) or events[0][1] != events[1][1]:
                    raise ValueError("saved chord does not match its intended MIDI notes")
                if source_rate != rate:
                    raise ValueError("chord fixture has the wrong sample rate")
                first, last = round(0.015 * rate), round(0.10 * rate)
                selected_notes = [note for note, _ in events] if expect["chord_poly"] else [events[-1][0]]
                reference = sum(source[(note - 36) * rate // 8 + first:(note - 36) * rate // 8 + last, 0]
                                for note in selected_notes)
                frame = events[0][1]
                correlations = np.array([
                    np.corrcoef(reference, audio[frame + first + shift:frame + last + shift, 0])[0, 1]
                    for shift in range(-8, 9)])
                correlations = np.nan_to_num(correlations, nan=-1)
                best = int(np.argmax(correlations)) - 8
                measured.update(chord_event_order=[note for note, _ in events], chord_offset_frames=best,
                                chord_correlation=float(np.max(correlations)))
                if abs(best) > 1 or np.max(correlations) < 0.98:
                    failures.append("chord output does not match the expected mono or poly voices")
            if expect.get("original_source_match") or expect.get("stem_source_match"):
                if source_rate != rate:
                    divisor = math.gcd(source_rate, rate)
                    source = resample_poly(source, rate // divisor, source_rate // divisor, axis=0)
                first, last = rate // 10, min(len(source), len(audio) - onset, int(3.9 * rate))
                reference = source[first:last, :2].reshape(-1)
                rendered = audio[onset + first:onset + last, :2].reshape(-1)
                denominator = float(np.dot(reference, reference))
                if denominator < 1e-12 or len(reference) != len(rendered):
                    raise ValueError("source comparison has insufficient audio")
                gain = float(np.dot(reference, rendered) / denominator)
                correlation = float(np.corrcoef(reference, rendered)[0, 1])
                measured.update(source_correlation=correlation, source_gain=gain)
                if not np.isfinite(correlation) or correlation < 0.99 or not 0.45 < gain < 0.55:
                    failures.append("render differs from the independently resampled reference source")
        except (OSError, RuntimeError, ValueError, ET.ParseError, KeyError, IndexError):
            failures.append("could not compare the saved MIDI event and source fixture")
    return finite_json({"name": case["name"], "status": "Failed" if failures else "Passed",
                        "failures": failures, "measurements": measured})


def compare_sample_stems(results: list[dict], folder: Path) -> None:
    by_name = {item["name"]: item for item in results}
    if "sample_original" not in by_name or by_name["sample_original"]["status"] != "Passed":
        for stem in ("vocals", "drums", "bass", "other"):
            result = by_name.get("sample_" + stem)
            if result:
                result["status"] = "Failed"
                result["failures"].append("passing original sample render is required to verify stem isolation")
        return
    original, rate = sf.read(folder / "sample_original.wav", dtype="float64", always_2d=True)
    for stem in ("vocals", "drums", "bass", "other"):
        name = "sample_" + stem
        if name not in by_name or by_name[name]["status"] != "Passed":
            continue
        solo, solo_rate = sf.read(folder / (name + ".wav"), dtype="float64", always_2d=True)
        result = by_name[name]
        if solo_rate != rate or solo.shape != original.shape:
            result["failures"].append("stem and original render formats differ")
        else:
            difference = rms(original - solo)
            result["measurements"]["difference_from_original_rms"] = difference
            if difference < max(1e-5, rms(original) * 0.01):
                result["failures"].append("solo render is indistinguishable from original mixture")
        if result["failures"]:
            result["status"] = "Failed"


def compare_related(results: list[dict], folder: Path) -> None:
    by_name = {item["name"]: item for item in results}
    for name, reference in (("cold_neutral", "neutral"), ("cold_host_gain_automation", "host_gain_automation"),
                            ("cold_host_tempo_transition", "host_tempo_transition"),
                            ("cold_sample_original", "sample_original"),
                            ("host_stem_automation", "sample_vocals"),
                            ("sample_solo_vocals", "sample_vocals")):
        if name not in by_name:
            continue
        result = by_name[name]
        if reference not in by_name:
            result["status"] = "Failed"
            result["failures"].append("reference render is required for this comparison")
            continue
        if result["status"] != "Passed":
            continue
        if by_name[reference]["status"] != "Passed":
            result["status"] = "Failed"
            result["failures"].append("reference render did not pass its own checks")
            continue
        actual, rate = sf.read(folder / (name + ".wav"), dtype="float64", always_2d=True)
        expected, expected_rate = sf.read(folder / (reference + ".wav"), dtype="float64", always_2d=True)
        if rate != expected_rate or actual.shape != expected.shape:
            result["failures"].append("comparison render formats differ")
        elif name in ("cold_neutral", "cold_host_gain_automation", "cold_host_tempo_transition",
                      "cold_sample_original", "sample_solo_vocals"):
            difference = np.abs(actual - expected)
            result["measurements"]["reference_max_difference"] = float(np.max(difference, initial=0))
            result["measurements"]["reference_rms_difference"] = rms(difference)
            if result["measurements"]["reference_max_difference"] > 1e-5:
                result["failures"].append("render differs from the reference by more than 1e-5")
        else:
            first = slice(int(0.10 * rate), int(0.40 * rate))
            last = slice(int(0.70 * rate), int(3.50 * rate))
            early_reference, late_reference = rms(expected[first]), rms(expected[last])
            result["measurements"]["reference_early_rms"] = early_reference
            result["measurements"]["reference_late_rms"] = late_reference
            result["measurements"]["automation_early_rms"] = rms(actual[first])
            result["measurements"]["automation_late_rms"] = rms(actual[last])
            if early_reference < 1e-5 or late_reference < 1e-5:
                result["failures"].append("vocal reference has insufficient energy to test stem automation")
            elif (rms(actual[first]) < early_reference * 0.8 or
                  rms(actual[first]) > early_reference * 1.2 or
                  rms(actual[last]) > late_reference * 0.05):
                result["failures"].append("stem gain automation did not preserve early audio and attenuate later audio")
        if result["failures"]:
            result["status"] = "Failed"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("cases", type=Path, nargs="?")
    parser.add_argument("render_dir", type=Path, nargs="?")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--midi-export", type=Path)
    args = parser.parse_args()
    if args.midi_export:
        try:
            print(json.dumps(inspect_midi(args.midi_export), indent=2))
            return 0
        except (OSError, ValueError) as exc:
            print("MIDI export check failed: " + str(exc), file=sys.stderr)
            return 1
    if args.cases is None or args.render_dir is None:
        parser.error("cases and render_dir are required unless --midi-export is provided")
    results = []
    for case in json.loads(args.cases.read_text()):
        try:
            results.append(check_case(case, args.render_dir / (case["name"] + ".wav")))
        except (OSError, RuntimeError, ValueError) as exc:
            results.append({"name": case["name"], "status": "Failed", "failures": [str(exc)]})
    compare_sample_stems(results, args.render_dir)
    compare_related(results, args.render_dir)
    payload = json.dumps(finite_json(results), indent=2, allow_nan=False) + "\n"
    if args.output:
        args.output.write_text(payload)
    print(payload, end="")
    return int(any(result["status"] != "Passed" for result in results))


if __name__ == "__main__":
    sys.exit(main())
