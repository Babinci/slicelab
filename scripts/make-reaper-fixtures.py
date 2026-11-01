#!/usr/bin/env python3
"""Create deterministic media and real serialized states for REAPER checks."""
from __future__ import annotations

import array
import copy
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import wave
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "local/verification"
RATE = 48000


def write_wave(path: Path, kind: str, seconds: float = 1.0) -> None:
    samples = array.array("h")
    for frame in range(round(RATE * seconds)):
        at = frame / RATE
        if kind == "tone":
            value = 0.4 * math.sin(2 * math.pi * 440 * at)
        elif kind == "click":
            phase = at % 0.5
            value = 0.6 * math.exp(-phase * 700) * math.sin(2 * math.pi * 1800 * phase)
        elif kind == "reverse":
            value = (0.15 if at < seconds / 2 else 0.65) * math.sin(2 * math.pi * 330 * at)
        elif kind == "coded":
            slot = min(31, int(at / seconds * 32))
            value = 0.3 * math.sin(2 * math.pi * (220 + slot * 30) * at)
        else:
            value = 0.0
        integer = round(value * 32767)
        samples.extend((integer, integer))
    if sys.byteorder != "little":
        samples.byteswap()
    with wave.open(str(path), "wb") as writer:
        writer.setparams((2, 2, RATE, 0, "NONE", "not compressed"))
        writer.writeframes(samples.tobytes())


def read_state(path: Path) -> ET.Element:
    data = path.read_bytes()
    magic, size = struct.unpack("<II", data[:8])
    if magic != 0x21324356 or size > len(data) - 8:
        raise ValueError("Invalid JUCE state fixture")
    return ET.fromstring(data[8:8 + size].rstrip(b"\0"))


def write_state(path: Path, state: ET.Element) -> None:
    xml = ET.tostring(state, encoding="utf-8")
    path.write_bytes(struct.pack("<II", 0x21324356, len(xml)) + xml + b"\0\0")


def configure(state: ET.Element, media: Path, parameters: dict, end: float = 1.0) -> ET.Element:
    result = copy.deepcopy(state)
    result.set("source", str(media))
    result.set("sourceHash", hashlib.sha256(media.read_bytes()).hexdigest())
    result.set("selectedPad", "0")
    result.set("stemManifest", "")
    result.set("viewWidth", "1240")
    result.set("viewHeight", "820")
    result.set("repitch", "0")
    anchors = result.find("ANCHORS")
    if anchors is not None:
        result.remove(anchors)
    for pad in result.find("PADS"):
        index = int(pad.get("index"))
        pad.set("start", "0")
        pad.set("end", str(end if index == 0 else 0))
        pad.set("favorite", "0")
    defaults = {"master_gain": 0, "source_bpm": 120, "target_bpm": 120,
                "sync": 0, "mono": 1, "hold": 0, "keyboard": 0,
                "global_semitones": 0, "global_cents": 0,
                "pad01_attack": 0, "pad01_release": 0,
                "pad01_gain": 0, "pad01_pan": 0, "pad01_semitones": 0,
                "pad01_cents": 0, "pad01_time": 1,
                "pad01_reverse": 0, "pad01_loop": 0, "pad01_output": 0}
    defaults.update(parameters)
    values = {item.get("id"): item for item in result.find("PARAMETERS")}
    for key, value in defaults.items():
        if key not in values:
            raise ValueError(f"Host parameter is missing: {key}")
        values[key].set("value", str(value))
    return result


def main() -> None:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    factory = read_state(OUTPUT / "factory.slicelab")
    fixtures = OUTPUT / "fixtures"
    fixtures.mkdir(exist_ok=True)
    for kind, seconds in (("tone", 1.0), ("click", 8.0), ("reverse", 1.0), ("coded", 4.0), ("silence", 2.0)):
        write_wave(fixtures / (kind + ".wav"), kind, seconds)
    cases = []
    specifications = [
        ("neutral", "tone", {}, 1.0, 1.3, {"frequency_hz": 440, "duration": 1.0}),
        ("pitch_octave", "tone", {"global_semitones": 12}, 1.0, 1.3, {"frequency_hz": 880, "duration": 1.0}),
        ("tempo_half", "tone", {"target_bpm": 60}, 1.0, 2.3, {"frequency_hz": 440, "duration": 2.0}),
        ("pitch_and_tempo", "tone", {"global_semitones": 12, "target_bpm": 60}, 1.0, 2.3, {"frequency_hz": 880, "duration": 2.0}),
        ("host_sync", "tone", {"sync": 1}, 1.0, 1.7, {"frequency_hz": 440, "duration": 4 / 3}),
        ("hold_release", "tone", {"hold": 1, "pad01_loop": 1}, 1.0, 1.0, {"stop_before": 0.35}),
        ("trigger_loop", "tone", {"hold": 0, "pad01_loop": 1}, 1.0, 1.0, {"audible_after": 0.75}),
        ("reverse", "reverse", {"pad01_reverse": 1}, 1.0, 1.3, {"first_half_louder": True}),
        ("pan_left", "tone", {"pad01_pan": -1}, 1.0, 1.3, {"right_silent": True}),
        ("all_stems_off", "tone", {f"stem_{stem}_enabled": 0 for stem in ("vocals", "drums", "bass", "other")}, 1.0, 1.3, {"silent": True}),
    ]
    for name, kind, parameters, end, length, expected in specifications:
        state = configure(factory, fixtures / (kind + ".wav"), parameters, end)
        path = OUTPUT / (name + ".slicelab")
        write_state(path, state)
        cases.append({"name": name, "state": str(path), "length": length,
                      "bpm": 90 if name == "host_sync" else 120,
                      "notes": [{"note": 36, "start": 0.02, "end": 0.22, "velocity": 127}],
                      "expected": expected})
    sample = OUTPUT / "sample.slicelab"
    if sample.exists():
        base = read_state(sample)
        for stem in ("original", "vocals", "drums", "bass", "other", "off"):
            state = copy.deepcopy(base)
            values = {item.get("id"): item for item in state.find("PARAMETERS")}
            for key, value in {"sync": 0, "target_bpm": 120, "source_bpm": 120, "master_gain": -6,
                               "pad01_attack": 0, "pad01_release": 0}.items():
                values[key].set("value", str(value))
            for name in ("vocals", "drums", "bass", "other"):
                values[f"stem_{name}_enabled"].set("value", "1" if stem in ("original", name) else "0")
                values[f"stem_{name}_gain"].set("value", "0")
            for item in state.find("PADS"):
                if item.get("index") == "0":
                    item.set("start", "0")
                    item.set("end", "4")
            path = OUTPUT / ("sample_" + stem + ".slicelab")
            write_state(path, state)
            cases.append({"name": "sample_" + stem, "state": str(path), "length": 4.3,
                          "bpm": 120, "notes": [{"note": 36, "start": 0.02, "end": 0.22, "velocity": 127}],
                          "expected": {"silent": True} if stem == "off" else {"audible": True}})
    (OUTPUT / "cases.json").write_text(json.dumps(cases, indent=2) + "\n")
    print(f"Created {len(cases)} REAPER cases in {OUTPUT}")


if __name__ == "__main__":
    main()
