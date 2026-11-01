import importlib.util
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
from types import SimpleNamespace

import numpy as np
import soundfile as sf
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/check-reaper-renders.py"
SPEC = importlib.util.spec_from_file_location("render_checks", SCRIPT)
render_checks = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(render_checks)
RUN_SPEC = importlib.util.spec_from_file_location("reaper_run", SCRIPT.with_name("run-reaper-verification.py"))
reaper_run = importlib.util.module_from_spec(RUN_SPEC)
RUN_SPEC.loader.exec_module(reaper_run)


class RenderChecksTest(unittest.TestCase):
    def test_final_stereo_bus_rejects_leakage_to_earlier_channels(self):
        rate = 48000
        signal = 0.3 * np.sin(2 * np.pi * 440 * np.arange(4800) / rate)
        audio = np.zeros((4800, 64))
        audio[:, 62:64] = signal[:, None]
        case = {"name": "last_bus", "length": 0.1, "channels": 64, "expected": {"last_aux_output": True}}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "last_bus.wav"
            sf.write(path, audio, rate, subtype="FLOAT")
            self.assertEqual(render_checks.check_case(case, path)["status"], "Passed")
            audio[:, 0] = signal
            sf.write(path, audio, rate, subtype="FLOAT")
            self.assertEqual(render_checks.check_case(case, path)["status"], "Failed")

    def test_render_with_nonfinite_audio_fails_with_valid_json(self):
        samples = np.zeros((4800, 2))
        samples[10, 0] = np.nan
        case = {"name": "nonfinite", "length": 0.1, "expected": {"silent": True}}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "nonfinite.wav"
            sf.write(path, samples, 48000, subtype="FLOAT")
            result = render_checks.check_case(case, path)
            self.assertEqual(result["status"], "Failed")
            self.assertEqual(result["measurements"]["nonfinite_samples"], 1)
            json.dumps(result, allow_nan=False)

    def test_exported_midi_requires_matched_note_events(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "export.mid"
            track = bytes([0, 0x90, 36, 100, 0x83, 0x60, 0x80, 36, 0, 0, 0xff, 0x2f, 0])
            header = b"MThd" + struct.pack(">IHHH", 6, 0, 1, 960)
            path.write_bytes(header + b"MTrk" + struct.pack(">I", len(track)) + track)
            info = render_checks.inspect_midi(path)
            self.assertEqual(info["matched_note_count"], 1)
            self.assertEqual(info["notes"][0]["note"], 36)
            wrong = bytearray(track)
            wrong[7] = 37
            path.write_bytes(header + b"MTrk" + struct.pack(">I", len(wrong)) + wrong)
            with self.assertRaises(ValueError):
                render_checks.inspect_midi(path)

    def test_window_capture_refuses_a_different_process(self):
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(reaper_run, "inspection_process", return_value=(Path(directory), {"pid": 123})):
                with mock.patch.object(reaper_run.subprocess, "run", return_value=SimpleNamespace(stdout="456\n")) as call:
                    with self.assertRaises(RuntimeError):
                        reaper_run.capture_gui(Path(directory), 1234)
                    self.assertEqual(call.call_count, 1)
                    self.assertEqual(call.call_args.args[0][:2], ["xdotool", "getwindowpid"])

    def test_gui_recording_refuses_an_unrelated_pipewire_node(self):
        objects = [{"id": 10, "type": "PipeWire:Interface:Node", "info": {"props":
                    {"application.process.id": "456", "node.name": "REAPER"}}}]
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(reaper_run, "inspection_process", return_value=(Path(directory), {"pid": 123})):
                with mock.patch.object(reaper_run.subprocess, "run", return_value=SimpleNamespace(stdout=json.dumps(objects).encode())):
                    with mock.patch.object(reaper_run.subprocess, "Popen") as spawn:
                        with self.assertRaises(RuntimeError):
                            reaper_run.record_gui(Path(directory), 3)
                        spawn.assert_not_called()

    def test_stretched_burst_requires_all_32_frequencies(self):
        rate = 48000
        frequencies = [(220 + index * 30) * 2 for index in range(32)]
        time = np.arange(6000) / rate
        tones = np.array([0.3 * np.sin(2 * np.pi * frequency * time) for frequency in frequencies])
        audio = np.zeros((round(0.3 * rate), 2))
        audio[960:6960] = tones.sum(axis=0)[:, None]
        case = {"name": "burst", "length": 0.3, "bpm": 120,
                "expected": {"burst_frequencies": frequencies}}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "projects").mkdir()
            (root / "renders").mkdir()
            lines = ["HASDATA 1 96000 QN"]
            lines.extend(f"E {3840 if index == 0 else 0} 90 {36 + index:02x} 7f" for index in range(32))
            (root / "projects/burst.RPP").write_text("\n".join(lines))
            path = root / "renders/burst.wav"
            sf.write(path, audio, rate, subtype="FLOAT")
            self.assertEqual(render_checks.check_case(case, path)["status"], "Passed")
            audio[960:6960] = (tones.sum(axis=0) - tones[17])[:, None]
            sf.write(path, audio, rate, subtype="FLOAT")
            result = render_checks.check_case(case, path)
            self.assertEqual(result["status"], "Failed")
            self.assertTrue(any("pad 18" in failure for failure in result["failures"]))

    def test_poly_chord_rejects_a_missing_voice(self):
        rate = 48000
        time = np.arange(4 * rate) / rate
        source = 0.3 * np.sin(2 * np.pi * (220 + np.minimum(31, (time * 8).astype(int)) * 30) * time)
        audio = np.zeros((round(0.3 * rate), 2))
        chord = source[:6000] + source[6000:12000]
        audio[960:6960] = chord[:, None]
        case = {"name": "chord", "length": 0.3, "bpm": 120,
                "expected": {"chord_notes": [36, 37], "chord_poly": True}}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "projects").mkdir()
            (root / "renders").mkdir()
            source_path = root / "source.wav"
            sf.write(source_path, np.column_stack((source, source)), rate, subtype="FLOAT")
            xml = f'<SliceLabState source="{source_path}"/>'.encode()
            state = root / "state.slicelab"
            state.write_bytes(b"VC2!" + struct.pack("<I", len(xml)) + xml + b"\0")
            case["state"] = str(state)
            (root / "projects/chord.RPP").write_text("HASDATA 1 96000 QN\nE 3840 90 24 7f\nE 0 90 25 7f\n")
            path = root / "renders/chord.wav"
            sf.write(path, audio, rate, subtype="FLOAT")
            self.assertEqual(render_checks.check_case(case, path)["status"], "Passed")
            audio[960:6960] = source[6000:12000, None]
            sf.write(path, audio, rate, subtype="FLOAT")
            self.assertEqual(render_checks.check_case(case, path)["status"], "Failed")
            case["expected"]["chord_poly"] = False
            self.assertEqual(render_checks.check_case(case, path)["status"], "Passed")

    def test_velocity_mode_requires_the_expected_level_relationship(self):
        rate = 48000
        audio = np.zeros((round(0.25 * rate), 2))
        wave = 0.4 * np.sin(2 * np.pi * 440 * np.arange(round(0.05 * rate)) / rate)
        for start, gain in ((0.02, 1.0), (0.12, 0.25)):
            frame = round(start * rate)
            audio[frame:frame + len(wave)] = (wave * gain)[:, None]
        case = {"name": "velocity", "length": 0.25, "expected": {"velocity_sensitive": True}}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "velocity.wav"
            sf.write(path, audio, rate, subtype="FLOAT")
            self.assertEqual(render_checks.check_case(case, path)["status"], "Passed")
            case["expected"]["velocity_sensitive"] = False
            self.assertEqual(render_checks.check_case(case, path)["status"], "Failed")

    def test_host_tempo_check_detects_extra_unsynchronized_beats(self):
        rate = 48000
        times = [0.02, 0.52, 1.04, 2.04, 3.04]
        audio = np.zeros((round(3.4 * rate), 2))
        pulse = 0.4 * np.hanning(96)
        for time in times:
            at = round(time * rate)
            audio[at:at + len(pulse)] = pulse[:, None]
        case = {"name": "tempo", "length": 3.4,
                "expected": {"tempo_pulses": times, "empty_pulse_midpoints": [1.54, 2.54]}}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "tempo.wav"
            sf.write(path, audio, rate, subtype="FLOAT")
            self.assertEqual(render_checks.check_case(case, path)["status"], "Passed")
            at = round(1.54 * rate)
            audio[at:at + len(pulse)] = pulse[:, None]
            sf.write(path, audio, rate, subtype="FLOAT")
            result = render_checks.check_case(case, path)
            self.assertEqual(result["status"], "Failed")
            self.assertTrue(any("unexpected pulse" in failure for failure in result["failures"]))

    def test_live_capture_checks_recorder_errors_and_complete_note(self):
        rate = 48000
        samples = np.sin(2 * np.pi * 440 * np.arange(60000) / rate) * 0.3
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            (run / "jobs").mkdir()
            sf.write(run / "reference.wav", samples, rate, subtype="FLOAT")
            sf.write(run / "jack.wav", np.column_stack((samples, samples)), rate, subtype="FLOAT")
            # The runner compares stereo buffers; provide a stereo reference too.
            sf.write(run / "reference.wav", np.column_stack((samples, samples)), rate, subtype="FLOAT")
            (run / "live-info.tsv").write_text("started\t10\nstopped\t13\nnote_on\t10.3\nnote_off\t10.5\n")
            (run / "jobs/live-capture.metadata.tsv").write_text("pdc_frames\t128\n")
            (run / "node-started").write_text("9.5")
            (run / "system-cpu.tsv").write_text("time\taggregate_pct\tmax_core_pct\n9\t10\t30\n11\t20\t40\n")

            def log(recorder_end):
                lines = []
                for timestamp in (9, 11, 12):
                    lines.append(f"{timestamp}\tR 83 512 48000 0 0 0 0 0 + REAPER\n")
                    err = recorder_end if timestamp == 12 else 0
                    lines.append(f"{timestamp}\tR 60 0 48000 0 0 0 0 {err} F32LE 2 48000 + capture\n")
                (run / "pw-top.log").write_text("".join(lines))

            log(0)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(reaper_run.evaluate_live(run, run / "reference.wav", "capture", 512, False, "virtual"), 0)
            result = json.loads((run / "live-analysis.json").read_text())
            self.assertEqual(result["reported_pdc_frames"], 128)
            self.assertEqual(result["pipewire_recorder_error_counter_end"], 0)
            log(1)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(reaper_run.evaluate_live(run, run / "reference.wav", "capture", 512, False, "virtual"), 1)
            self.assertTrue(any("recorder error counter" in failure
                                for failure in json.loads((run / "live-analysis.json").read_text())["failures"]))
            log(0)
            corrupt = np.column_stack((samples, samples))
            corrupt[10000, 0] = np.nan
            sf.write(run / "jack.wav", corrupt, rate, subtype="FLOAT")
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(reaper_run.evaluate_live(run, run / "reference.wav", "capture", 512, False, "virtual"), 1)
            result = json.loads((run / "live-analysis.json").read_text(),
                                parse_constant=lambda value: self.fail("non-finite JSON value: " + value))
            self.assertTrue(any("NaN or infinity" in failure for failure in result["failures"]))

    def test_all_pad_mapping_detects_the_wrong_slice(self):
        rate = 48000
        frames = [960 + index * 7680 for index in range(32)]
        bounds = [[index / 8, (index + 1) / 8] for index in range(32)]
        time = np.arange(4 * rate) / rate
        slots = np.minimum(31, (time * 8).astype(int))
        source = 0.3 * np.sin(2 * np.pi * (220 + slots * 30) * time)
        audio = np.zeros((round(5.2 * rate), 2))
        for index, frame in enumerate(frames):
            chunk = source[index * 6000:(index + 1) * 6000]
            audio[frame:frame + 6000] = chunk[:, None]
        case = {"name": "pads", "length": 5.2, "bpm": 120,
                "expected": {"pad_frames": frames, "pad_bounds": bounds}}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "projects").mkdir()
            (root / "renders").mkdir()
            source_path = root / "source.wav"
            sf.write(source_path, np.column_stack((source, source)), rate, subtype="FLOAT")
            xml = f'<SliceLabState source="{source_path}"/>' .encode()
            state = root / "state.slicelab"
            state.write_bytes(b"VC2!" + struct.pack("<I", len(xml)) + xml + b"\0")
            case["state"] = str(state)
            events = []
            for index, frame in enumerate(frames):
                events.extend(((frame, 0x90, 36 + index, 127), (frame + 2400, 0x80, 36 + index, 0)))
            previous = 0
            lines = ["HASDATA 1 96000 QN"]
            for frame, status, note, velocity in events:
                lines.append(f"E {(frame - previous) * 4} {status:02x} {note:02x} {velocity:02x}")
                previous = frame
            (root / "projects/pads.RPP").write_text("\n".join(lines))
            self.assertEqual(render_checks.midi_note_events(case, root / "renders", rate),
                             [(36 + index, frame) for index, frame in enumerate(frames)])
            path = root / "renders/pads.wav"
            sf.write(path, audio, rate, subtype="FLOAT")
            result = render_checks.check_case(case, path)
            self.assertEqual(result["status"], "Passed", result["failures"])
            frame = frames[17]
            audio[frame:frame + 6000] = source[18 * 6000:19 * 6000, None]
            sf.write(path, audio, rate, subtype="FLOAT")
            result = render_checks.check_case(case, path)
            self.assertEqual(result["status"], "Failed")
            self.assertTrue(any("pad 18" in failure for failure in result["failures"]))

    def test_process_guard_refuses_the_wrong_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            profile = Path(directory)
            process = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)", "--",
                                        "-newinst", "-cfgfile", str(profile / "reaper.ini")])
            previous = reaper_run.REAPER
            try:
                reaper_run.REAPER = Path(sys.executable).resolve()
                self.assertEqual(reaper_run.verify_child(process, profile)["pid"], process.pid)
                with self.assertRaises(RuntimeError):
                    reaper_run.ensure_profile_unused(profile)
                reaper_run.ensure_profile_unused(profile / "other")
                with self.assertRaises(RuntimeError):
                    reaper_run.stop_child(process, profile / "other")
                self.assertIsNone(process.poll())
                reaper_run.stop_child(process, profile)
                self.assertIsNotNone(process.poll())
                reaper_run.ensure_profile_unused(profile)
            finally:
                reaper_run.REAPER = previous
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_component_replacement_keeps_host_frame_and_checks_restore(self):
        def state(value):
            xml = (f'<SliceLabState source="/source.wav" sourceHash="" stemManifest="">'
                   f'<PADS><PAD index="0" start="0" end="1"/></PADS>'
                   f'<PARAMETERS><PARAM id="gain" value="{value}"/></PARAMETERS></SliceLabState>').encode()
            return b"VC2!" + struct.pack("<I", len(xml)) + xml + b"\0"
        old, new = state("0"), state("-12.5")
        wrapped = struct.pack("<II", len(old) + 12, 1) + old + b"private-data" + b"controller-data"
        result = reaper_run.replace_component(wrapped, new + b"\0")
        self.assertEqual(struct.unpack_from("<I", result)[0], len(new) + 12)
        self.assertTrue(result.endswith(b"controller-data"))
        reaper_run.check_restored(new, result)
        with self.assertRaises(ValueError):
            reaper_run.check_restored(old, result)

    def test_frequency_duration_and_silence_detect_wrong_output(self):
        rate = 48000
        samples = np.zeros((int(1.3 * rate), 2))
        time = np.arange(rate) / rate
        samples[960:960 + rate] = (0.3 * np.sin(2 * np.pi * 440 * time))[:, None]
        case = {"name": "tone", "length": 1.3,
                "expected": {"frequency_hz": 440, "duration": 1.0}}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "render.wav"
            sf.write(path, samples, rate, subtype="FLOAT")
            self.assertEqual(render_checks.check_case(case, path)["status"], "Passed")
            case["expected"]["frequency_hz"] = 880
            self.assertEqual(render_checks.check_case(case, path)["status"], "Failed")
            samples[960:960 + rate] = (0.3 * np.sin(2 * np.pi * 442 * time))[:, None]
            sf.write(path, samples, rate, subtype="FLOAT")
            case["expected"]["frequency_hz"] = 440
            self.assertEqual(render_checks.check_case(case, path)["status"], "Failed")
            half = rate // 2
            samples[960:960 + half] = (0.3 * np.sin(2 * np.pi * 440 * time[:half]))[:, None]
            samples[960 + half:960 + rate] = (0.3 * np.sin(2 * np.pi * 447 * time[half:]))[:, None]
            sf.write(path, samples, rate, subtype="FLOAT")
            self.assertEqual(render_checks.check_case(case, path)["status"], "Failed")
            case["expected"] = {"silent": True}
            self.assertEqual(render_checks.check_case(case, path)["status"], "Failed")
            samples[:] = 0
            sf.write(path, samples, rate, subtype="FLOAT")
            self.assertEqual(render_checks.check_case(case, path)["status"], "Passed")
            case["expected"] = {"frequency_hz": 440, "duration": 1.0}
            self.assertEqual(render_checks.check_case(case, path)["status"], "Failed")

    def test_reverse_and_pan_have_observable_polarity(self):
        rate = 48000
        t = np.arange(rate) / rate
        samples = np.zeros((int(1.3 * rate), 2))
        samples[960:960 + rate, 0] = np.sin(2 * np.pi * 330 * t) * np.where(t < 0.5, 0.6, 0.1)
        case = {"name": "reverse", "length": 1.3,
                "expected": {"first_half_louder": True, "right_silent": True}}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "render.wav"
            sf.write(path, samples, rate, subtype="FLOAT")
            self.assertEqual(render_checks.check_case(case, path)["status"], "Passed")
            samples[:, 1] = samples[:, 0]
            sf.write(path, samples, rate, subtype="FLOAT")
            self.assertEqual(render_checks.check_case(case, path)["status"], "Failed")

    def test_auxiliary_output_and_cold_restore_detect_leakage_or_change(self):
        rate = 48000
        time = np.arange(rate) / rate
        samples = np.zeros((int(1.3 * rate), 4))
        samples[960:960 + rate, 2:] = (0.3 * np.sin(2 * np.pi * 440 * time))[:, None]
        case = {"name": "aux_output", "length": 1.3, "channels": 4,
                "expected": {"aux_output": True, "frequency_hz": 440}}
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            path = folder / "aux_output.wav"
            sf.write(path, samples, rate, subtype="FLOAT")
            self.assertEqual(render_checks.check_case(case, path)["status"], "Passed")
            samples[:, :2] = samples[:, 2:]
            sf.write(path, samples, rate, subtype="FLOAT")
            self.assertEqual(render_checks.check_case(case, path)["status"], "Failed")
            sf.write(folder / "neutral.wav", samples[:, :2], rate, subtype="FLOAT")
            sf.write(folder / "cold_neutral.wav", samples[:, :2] * 0.5, rate, subtype="FLOAT")
            results = [{"name": name, "status": "Passed", "failures": [], "measurements": {}}
                       for name in ("neutral", "cold_neutral")]
            render_checks.compare_related(results, folder)
            self.assertEqual(results[1]["status"], "Failed")

    def test_saved_midi_alignment_and_original_source_match(self):
        rate = 48000
        t = np.arange(4 * rate) / rate
        source = np.column_stack([0.3 * np.sin(2 * np.pi * 440 * t)] * 2)
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / "projects").mkdir()
            (folder / "renders").mkdir()
            source_path = folder / "source.wav"
            sf.write(source_path, source, rate, subtype="FLOAT")
            root = ET.Element("SliceLabState", source=str(source_path))
            xml = ET.tostring(root)
            state = folder / "source.slicelab"
            state.write_bytes(b"VC2!" + struct.pack("<I", len(xml)) + xml + b"\0")
            for name in ("neutral", "sample_original"):
                (folder / "projects" / (name + ".RPP")).write_text("HASDATA 1 960 QN\nE 38 90 24 7f\n")
            rendered = np.zeros((int(4.3 * rate), 2))
            rendered[950:950 + len(source)] = source * 0.501187
            neutral = {"name": "neutral", "length": 4.3, "bpm": 120, "state": str(state),
                       "expected": {"timing_correlation": True, "exact_source_extent": True}}
            path = folder / "renders/neutral.wav"
            sf.write(path, rendered, rate, subtype="FLOAT")
            self.assertEqual(render_checks.check_case(neutral, path)["status"], "Passed")
            truncated = rendered.copy()
            truncated[950 + len(source) - 3:950 + len(source)] = 0
            sf.write(path, truncated, rate, subtype="FLOAT")
            self.assertEqual(render_checks.check_case(neutral, path)["status"], "Failed")
            sf.write(path, np.roll(rendered, 4, axis=0), rate, subtype="FLOAT")
            self.assertEqual(render_checks.check_case(neutral, path)["status"], "Failed")
            original = {"name": "sample_original", "length": 4.3, "bpm": 120, "state": str(state),
                        "expected": {"original_source_match": True}}
            path = folder / "renders/sample_original.wav"
            sf.write(path, rendered, rate, subtype="FLOAT")
            self.assertEqual(render_checks.check_case(original, path)["status"], "Passed")
            sf.write(path, np.roll(rendered, 20, axis=0), rate, subtype="FLOAT")
            self.assertEqual(render_checks.check_case(original, path)["status"], "Failed")

    def test_stem_reference_detects_mislabeled_audio(self):
        rate = 48000
        time = np.arange(4 * rate) / rate
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / "projects").mkdir()
            (folder / "renders").mkdir()
            stems = {}
            for name, frequency in (("vocals", 440), ("bass", 220)):
                path = folder / (name + ".wav")
                samples = np.column_stack([0.3 * np.sin(2 * np.pi * frequency * time)] * 2)
                sf.write(path, samples, rate, subtype="FLOAT")
                stems[name] = str(path)
            manifest = folder / "manifest.json"
            manifest.write_text(json.dumps({"stems": stems}))
            xml = ET.tostring(ET.Element("SliceLabState", source=stems["bass"], stemManifest=str(manifest)))
            state = folder / "source.slicelab"
            state.write_bytes(b"VC2!" + struct.pack("<I", len(xml)) + xml + b"\0")
            (folder / "projects/sample_vocals.RPP").write_text("HASDATA 1 960 QN\nE 38 90 24 7f\n")
            rendered = np.zeros((int(4.3 * rate), 2))
            rendered[950:950 + len(time)] = (0.3 * 0.501187 * np.sin(2 * np.pi * 440 * time))[:, None]
            path = folder / "renders/sample_vocals.wav"
            sf.write(path, rendered, rate, subtype="FLOAT")
            case = {"name": "sample_vocals", "length": 4.3, "bpm": 120, "state": str(state),
                    "expected": {"stem_source_match": "vocals"}}
            self.assertEqual(render_checks.check_case(case, path)["status"], "Passed")
            case["expected"]["stem_source_match"] = "bass"
            self.assertEqual(render_checks.check_case(case, path)["status"], "Failed")

    def test_midi_offsets_detect_a_shift_at_one_event(self):
        rate = 48000
        frames = [i * 5120 + x for i, x in enumerate((17, 63, 64, 127, 128, 129))]
        time = np.arange(rate) / rate
        tone = 0.3 * np.sin(2 * np.pi * 440 * time)
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / "projects").mkdir()
            (folder / "renders").mkdir()
            source = folder / "tone.wav"
            sf.write(source, np.column_stack([tone, tone]), rate, subtype="FLOAT")
            xml = ET.tostring(ET.Element("SliceLabState", source=str(source)))
            state = folder / "source.slicelab"
            state.write_bytes(b"VC2!" + struct.pack("<I", len(xml)) + xml + b"\0")
            events = []
            for frame in frames:
                events.extend([(frame * 4, "90 24 7f"), ((frame + 240) * 4, "80 24 00")])
            previous = 0
            lines = ["HASDATA 1 96000 QN"]
            for tick, message in events:
                lines.append(f"E {tick - previous} {message}")
                previous = tick
            (folder / "projects/midi_offsets.RPP").write_text("\n".join(lines) + "\n")
            audio = np.zeros((round(0.65 * rate), 2))
            for frame in frames:
                audio[frame:frame + 2400] = tone[:2400, None]
            path = folder / "renders/midi_offsets.wav"
            sf.write(path, audio, rate, subtype="FLOAT")
            case = {"name": "midi_offsets", "length": 0.65, "bpm": 120, "state": str(state),
                    "expected": {"midi_frames": frames}}
            self.assertEqual(render_checks.check_case(case, path)["status"], "Passed")
            shifted = audio.copy()
            frame = frames[3]
            shifted[frame:frame + 2405] = 0
            shifted[frame + 4:frame + 2404] = tone[:2400, None]
            sf.write(path, shifted, rate, subtype="FLOAT")
            self.assertEqual(render_checks.check_case(case, path)["status"], "Failed")


if __name__ == "__main__":
    unittest.main()
