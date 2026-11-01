# SliceLab sampler engine

This document records the implementation and its current verification limits. The Rubber Band callback still requires a real-time allocation fix before the engine can be called production ready.

## Playback and ownership

`SamplerEngine` uses 32 prepared voice slots. A voice captures a source asset and a half-open slice interval in source frames. MIDI notes 36 through 67 select the 32 pads. Keyboard mode plays the selected pad relative to its root note. UI preview uses a separate flagged voice and does not change pad settings.

The processor publishes immutable source and boundary views. The audio thread protects the acquired view and each active voice with atomic hazard pointers. A worker or message thread reclaims a retired view after those pointers move away from it. An old voice can finish against its captured source after a new source is published.

The audio callback handles MIDI at its block offset and uses bounded queues for UI events. It maps disabled auxiliary outputs back to the main stereo pair. Each pad has gain, pan, pitch, time ratio, filter, attack, release, reverse, loop, and output parameters. Source-rate conversion is independent of the host rate. Non-looping slice ends receive a short fade. A source interval shorter than one frame is quantized to one frame, and loop wrap uses constant-time modulo.

In Stretch mode, the time ratio is the pad ratio multiplied by source BPM divided by target BPM. Host sync selects host BPM times the host multiplier when the host supplies a tempo. Otherwise the saved target BPM is used. Repitch mode couples rate and pitch. The current engine limits the combined time ratio and pitch factor to the range 0.25 through 4. Extreme combinations of the individual controls can therefore reach that limit before the displayed target does. Key override records a correction to the detected source key. It does not implement a separate target-key selector.

Rubber Band receives input from the in-memory source in bounded chunks. Real-time startup supplies the library's preferred input padding and discards its reported output delay before emitting audio. Unity time and pitch use direct playback. The engine prepares and warms every stretcher outside the audio callback. A no-op real-time logger avoids the library's default stream logging in that callback.

## Media and state

The worker decodes the source and checks its file hash before and after decoding. It computes waveform peaks, an onset envelope and tempo estimate, and a chroma key estimate. Silence leaves BPM at zero and key at `Unknown`. The global source-BPM control retains its editable 120 BPM fallback when detection is unavailable.

The separation worker is invoked with an argument array. A completed result must match the source hash, sample rate, and frame count. The processor verifies available stem checksums and decodes all four aligned files before publishing a set. It reverses the input and output processing gains recorded in the manifest. The original source is the all-enabled unity mix. Mutes, solos, and levels control the separated estimates. Changing the solo mask does not erase the mute mask.

Project state uses a versioned JUCE binary XML document. It contains the source path and SHA-256, 32 pad intervals and names, host parameters, beat anchors, the stem manifest path, and view state. A missing or modified source leaves the saved session available for relinking. `relinkSample` rejects a file with a different hash. `loadSample` is the replacement operation. A failed import leaves the previous playable source in place. Collect verifies source and stem checksums before changing the session paths.

Host parameter IDs use `master_gain`, `source_bpm`, `target_bpm`, `global_semitones`, `global_cents`, `host_multiplier`, `sync`, `hold`, `mono`, `keyboard`, `repitch`, `velocity_sensitive`, `glide_ms`, and `root_note`. Stem IDs follow `stem_<name>_enabled`, `stem_<name>_gain`, and `stem_<name>_solo`. Pad IDs follow `pad01_<field>` through `pad32_<field>`, with fields `gain`, `pan`, `semitones`, `cents`, `time`, `cutoff`, `resonance`, `attack`, `release`, `reverse`, `loop`, and `output`.

## Verification and open gates

`tests/engine_tests.cpp` exercises sample-offset MIDI, pitch and time behavior, 44.1, 48, and 96 kHz processing, reverse and loop playback, note release, sustain, simultaneous pads, source preview, routing, source retention, state recovery, relinking, analysis, and a real stem result when the private sample is available. Setting `SLICELAB_FIXTURE_DIR` also writes processor state fixtures to that local directory. The built executable completed with zero failures on 2026-11-01. It loaded the private sample and the cached four-stem result. The run wrote `factory.slicelab` and `sample.slicelab` under `local/verification`. The command used the `SliceLabEngineTests` executable from `build/plugins/SliceLab/SliceLabEngineTests_artefacts/Release` with `SLICELAB_FIXTURE_DIR` set to that directory. A second run added mute, solo, tempo, and pitch changes during an active voice. It also completed with zero failures. The captured outputs are in `local/engine-tests.log` and `local/engine-tests-continuous.log`.

The local probe `local/verification/astra-rubberband-allocations.cpp` intercepted the installed Rubber Band 3.3 library. A cold pitch change to 0.25 made four allocation calls and 33 free calls. After warming, some pitch and time-ratio transitions still called `free`. The current warm-up reduces the cold-start exposure but does not prove a heap-free callback. Audio-thread allocation and deallocation, worst-case voice CPU, listening quality, and REAPER host behavior remain release gates until tested and fixed.
