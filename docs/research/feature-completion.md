# SliceLab feature completion

## Slicing and key controls

Suggest ranks short source windows by their energy, energy change, and waveform change. It selects prominent events with a minimum spacing and deterministic ordering. Transients remains a separate onset-based mode. Both modes preserve favorite pads and support undo.

Grid slicing stores its interval in beats. The editor offers 1/16, 1/8, 1/4, 1/2, 1, 2, and 4 beats. The processor clamps restored or API-supplied intervals to 1/16 through 4 beats. Piecewise beat anchors and the grid offset determine the source time of each grid line.

The source-key control corrects the detected tonic and major or minor mode. Replacing the source clears the previous source's manual correction. The target-key control selects a tonic and writes the nearest semitone interval to the automatable global pitch parameter. It is disabled while the source key is unknown. The status line computes the selected pad's audible key from global and pad tuning, fine tuning, and the current repitch rate. Pad tuning and repitch can therefore move the audible key away from the target tonic.

## Optional key analyzer

The native chroma estimate is available after the sample loads. If `~/.local/share/reaper-key-detection/analyze_key.py` and its `.venv/bin/python` exist, SliceLab also starts that installed S-KEY and Essentia analyzer on the background worker. It accepts only a validated key and `high`, `medium`, or `low` confidence from the tool's `KEY_RESULT` TSV output. Low confidence leaves the key unknown. A timeout, unavailable runtime, changed file, or analyzer failure leaves the native estimate in place. No model is downloaded.

The analyzer runs for sources between 0.75 and 180 seconds and has a 45-second timeout. A source replacement cancels stale analysis. `SLICELAB_KEY_ANALYZER` can point to a compatible script or contain `off` to disable it. `SLICELAB_KEY_PYTHON` can select that script's Python executable. Without that override, the executable is `.venv/bin/python` beside the script.

## Memory limits

A decoded source is limited to 128 MiB of stereo float PCM. A source plus its four decoded stereo stems is limited to 256 MiB. Oversized imports fail before allocating the decoded buffer. Preset restore checks these limits before replacing the session. Stem files are decoded into a temporary set and published only after every stem passes validation. The previous source remains playable when an import or separation fails.

Undo and redo retain at most 64 edits and 256 MiB of unique non-current PCM assets. Edits to pads share the same immutable asset. Playing voices can retain an older asset until the voice ends, so the history budget is not a bound on every live audio buffer. New load requests replace queued stale loads before decoding, and worker exceptions become an error instead of terminating the process.

## Verification

`tests/processor_feature_tests.cpp` exercises generated WAV, AIFF, FLAC, Ogg, and MP3 files at Unicode paths. It checks Suggest, favorites, undo and redo, fractional grids with anchors, state round trips, key transposition, rendered pitch, oversized source handling, and the analyzer output contract. `ffmpeg` is needed to generate the compressed fixtures.

Set `SLICELAB_VERIFY_MEMORY=1` to run the larger history and stem-budget cases. Set `SLICELAB_VERIFY_EXTERNAL_KEY=1` to exercise the installed analyzer against `sample_test.wav`. The editor still requires host-driven layout and interaction verification at its minimum size.

On 2026-11-01, the CMake-built feature test completed with both options enabled and zero failures. The installed analyzer reported F major with high confidence for `sample_test.wav`. The output is saved in `local/verification/processor-feature-tests.log`.
