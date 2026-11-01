# REAPER render results

## Baseline on 2026-11-01

This is a baseline, not a release verdict. The installed Linux VST3 had SHA-256 `f0d6db77076304abd0111441b09307d6703eb248b5ee529a770ea3d1ff20edb4`. REAPER 7.78 loaded that exact shared library in its own process and identified it as `VST3i: SliceLab (SliceLab)`. The plugin reported 64 output pins and zero frames of delay compensation. REAPER exposed 410 named SliceLab parameters, its MIDI controller parameters, and wrapper controls. The full host parameter identifiers are in the private evidence directory.

The isolated REAPER profile had no open audio device. The WAV renders were 48 kHz, 32-bit float. The physical audio backend, negotiated device rate, and buffer size were unavailable. REAPER also appended its standard `~/.vst3` directory to the private scan path. The tested binary's process mapping and hash were checked against the installed bundle.

The saved projects, MIDI items, states, source-dependent audio, process records, and measurements are under `local/verification/reaper-runs/20261101-132235-daa8e472/`. That directory is ignored by Git. The rechecked results are in `audio-recheck.json`. The current runner writes a separate private resource directory and a new evidence directory for each run.

| Case | Result | Evidence |
| --- | --- | --- |
| Neutral tone and MIDI | Passed | The saved note began at frame 950. Waveform correlation found a zero-frame offset and correlation 0.99994. |
| One octave up | Failed | The target was 880 Hz. The largest dominant spectral peak deviation was +10.30 cents. |
| Half tempo | Failed | The audible duration was 2.01 s for a 2 s target. The largest dominant spectral peak deviation was +12.74 cents. |
| Octave up and half tempo | Failed | The audible duration was 2.01 s. The largest dominant spectral peak deviation was +6.87 cents. |
| Host sync at 90 BPM | Failed | The audible duration was 1.35 s for a 1.333 s target. The largest dominant spectral peak deviation was +9.81 cents. |
| Hold, trigger loop, reverse, and left pan | Passed | Release reached silence, the loop remained audible, the reversed loud section came first, and the right channel was silent at full-left pan. |
| Synthetic and real-sample all-stems-off | Passed | Both render peaks were exactly zero. |
| Original and four isolated real stems | Passed | Independent source resampling gave correlations of 0.99808 for the original and at least 0.99710 for each matching stem. |
| Master and vocal-stem gain automation | Passed | A REAPER envelope reduced the master and vocal stem to approximately 0.1% of their reference RMS levels. |
| Pad output 2 | Passed | A four-channel render contained silence on channels 1 and 2 and the signal on channels 3 and 4. |
| Cold neutral reopen | Passed with limited timing coverage | The fresh-process render had sample-identical PCM. Parameter enumeration occurred before the render, so this run does not prove the strict immediate-render requirement. |
| Vocal solo control | Passed in a separate run | Enabling the actual vocal solo parameter produced the same PCM as the manually isolated vocal and correlated with its source stem. |
| MIDI event positions | Passed in a separate run | Six saved MIDI events at frames 17, 5183, 10304, 15487, 20608, and 25729 each matched the source waveform at zero-frame offset. The actual offline callback size was not established. |

The four pitch cases exceeded the verification plan's five-cent target for the dominant spectral peak. The checker measures successive 400 ms Hann-windowed tone regions and uses zero-padding to obtain 0.25 Hz frequency-bin spacing. A zero-padded bin size is not a guarantee of estimator accuracy. A separate band-limited instantaneous-frequency estimate was near the expected frequency in several failing regions; for the octave case a sideband around 885.25 Hz was stronger than the component near 880 Hz. These observations do not establish a constant tuning error. The spectral results require another render against the final DSP binary.

The audible-duration check uses 10 ms RMS windows and a tolerance of the larger of 80 ms or 5% of the target. The neutral tone's first and last samples above `1e-5` also matched the independently calculated source extent exactly, at frames 951 and 48949. This thresholded neutral check does not establish one-frame scheduled-end accounting for transformed audio or separately identify an envelope tail.

The source of the real-stem cases was the private separation manifest. It names `model_bs_roformer_ep_17_sdr_9.6568.ckpt` with checkpoint SHA-256 `3e9daecd70aaed5b5a0d1f861cc4d77eaa45afb3fc6301b1cf32c1be0f5868fb`. Each baseline stem case enabled one stem and disabled the others. These comparisons establish transport and labeling for this manifest; they do not measure separation quality. The separate solo-control run is `20261101-133503-c9fd2fda`; the corrected MIDI offset run is `20261101-133604-a06c57f2`.

## Baseline real-time comparison

These short tests used an isolated JACK client backed by PipeWire at 48 kHz. REAPER played a saved MIDI item or received a note from its virtual MIDI keyboard. A private PipeWire recorder captured the output. The graph control played the same synthetic audio as an ordinary media item with SliceLab bypassed. CPU percentages were sampled from `/proc/stat` at 0.2-second intervals. A transparent post-instrument effect was used only in runs labeled instrumented; its callback timestamps are a host-track observation, not the VST3's processing-mode flag.

| Buffer | Input and observation | Result |
| --- | --- | --- |
| 128 frames | Graph control, minimal instrumentation | One short run matched exactly; another lost a 128-frame block despite zero sampled REAPER and recorder error counters. |
| 128 frames | MIDI item, neutral | One run matched exactly with zero sampled REAPER errors; another had six sampled errors and an audio mismatch during a 90% aggregate CPU spike while an old inspection instance was still open. |
| 128 frames | MIDI item, octave up | A minimal run matched the first 0.8 s but recorded six sampled REAPER errors. An instrumented run matched the offline signal at the track tap but the JACK capture differed; it had two sampled REAPER and recorder errors during a 97.5% aggregate CPU spike. |
| 512 frames | Graph control, neutral and octave-up MIDI items | Each of the three minimal runs matched the first 0.8 s exactly, with zero sampled REAPER and recorder errors during 2.7 seconds of playback. |
| 512 frames | Octave-up MIDI item, instrumented | The track tap and JACK capture matched the reference for the first 0.8 s, with zero sampled errors. Median track callback interval was 10.71 ms; the maximum was 14.70 ms. |
| 512 frames | Virtual MIDI, neutral and octave up | After a 0.8 s wait for new graph links to settle, both short runs matched the first 0.8 s and had zero sampled errors. Two earlier octave-up attempts had capture discontinuities; one also reported two REAPER errors. |

The two virtual MIDI passes are `live-20261101-134833-3fb489a3` and `live-20261101-134842-286aa100` under the ignored evidence directory. The 512-frame results are useful for choosing the next test configuration, but brief passing runs do not establish uninterrupted long-term playback. A zero sampled error count does not prove the recorder never dropped audio. The updated runner compares up to a full second of reference audio and checks the recorder's own sampled errors as well as REAPER's.

The tests loaded serialized state through REAPER's VST3 state API and rendered real MIDI items with the editor closed. They did not perform native file import, drag and drop, mouse gestures, visual resizing, live tempo automation, or callback-boundary identification. They also did not compare all supported bus numbers or verify project portability with missing media. GUI inspection is a separate activity.

The standalone helper tests can be run with `.toolchain/bin/python -m unittest discover -s tests -p 'test_reaper_render_checks.py' -v`. After generating the fixture states, run `.toolchain/bin/python scripts/run-reaper-verification.py`. The runner fails if a render misses an expectation, the installed binary changes during the run, the child process does not map that binary, or the expected resource and project identities do not match. It terminates only the process it started after checking its PID, parent, executable, `-newinst` flag, and private configuration path.

For a live check, pass `--live-run` with the private run directory, `--live-case pitch_octave`, `--live-source virtual`, `--disable-anticipative`, `--quantum 512`, and optionally `--instrumented`. The runner creates and tears down its own PipeWire recorder and REAPER child. A separate inspection copy can be opened with `--gui-run` and `--pipewire`; it is intentionally left open and prints its PID, private profile, project, and snapshot directory. `--gui-snapshot` with that directory records the current plugin state; adding `--gui-save` saves the owned project first. After closing the owned session, `--gui-run` and `--gui-source` can reopen a copy of that saved private project. Inspection sessions must be treated separately from the user's normal REAPER project.

For an editor gesture that produces sound, `--gui-snapshot <snapshot-directory> --gui-record --record-seconds 8` records the owned REAPER node into that private directory. The helper identifies the node by process ID and refuses to connect an unrelated PipeWire node. A screenshot can be captured with `--gui-snapshot <snapshot-directory> --gui-capture --gui-window <window-id>`; the window's process ID must match the inspection session.

To inspect a MIDI file exported during GUI testing, use `.toolchain/bin/python scripts/check-reaper-renders.py --midi-export <private-file.mid>`. The check parses Standard MIDI File events and requires at least one matching note-on and note-off.
