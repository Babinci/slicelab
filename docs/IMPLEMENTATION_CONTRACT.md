# SliceLab implementation contract

## Chosen structure

Use a native JUCE editor and a fixed voice pool over an immutable source asset. This combines the direct Linux input and host behavior from the native candidate with the explicit state model from the Astra product design. Audio Plugin Coder remains the pinned framework, root build project, and lifecycle record.

A bank-only stretch engine is rejected. Host tempo automation and offline render cannot wait for an asynchronous cache. Use a prepared, bounded Rubber Band real-time pull path over prerecorded memory. Preallocate stretchers and working buffers outside `processBlock`. Source lookahead is available because this is a sampler. A prepared cache can be added only as an optimization. Independent tempo and pitch must work on the live path.

## Ownership

- Root owns `Source/Domain.h`, the build and install scripts, product documentation, and final integration.
- The engine worker owns `Source/SamplerEngine.*`, `Source/PluginProcessor.*`, and engine behavior tests. It may add private engine files. It must preserve the public contract below.
- The editor worker owns `Source/PluginEditor.*` and native GUI helper files. It must use the public processor API below.
- The stem worker owns `scripts/stem_worker.py`, `scripts/remote_worker.py`, and its protocol tests. C++ starts its local CLI on a background thread.

All paths above are relative to `plugins/SliceLab` except the root scripts and tests. Workers do not commit or edit another worker's files. Report a needed contract change to root before making it.

## State and invariants

`SampleAsset` owns original stereo PCM, source rate, source identity, waveform peaks, transient source positions, detected BPM and musical key, and an optional aligned four-stem set. A new load has a new asset generation. Old asynchronous results cannot replace that generation.

`SessionState` owns 32 stable pad slots and global playback settings. Pad boundaries are seconds in the original source and use a half-open interval. Tempo and pitch never rewrite the boundaries. MIDI pads are 36 through 67. Empty pads have `endSeconds <= startSeconds`. Output zero is the main stereo bus. Outputs one through 31 are optional auxiliary stereo buses.

`Snapshot` is a non-realtime UI copy containing the state, asset, status, progress, host BPM, selected pad, voice activity, and undo readiness. The UI must not mutate asset memory. The callback must not decode, allocate, launch processes, access the network, or block on a mutex. Retire immutable assets off the audio callback after no voice can use them.

## Public processor API

The class is `SliceLabAudioProcessor`. Its editor is `SliceLabAudioProcessorEditor`. All user commands below run from the message thread. `snapshot()` returns `slicelab::Snapshot`.

```cpp
slicelab::Snapshot snapshot() const;
void loadSample(const juce::File&);
void setGlobal(const slicelab::GlobalSettings&);
void setPad(int index, const slicelab::PadSettings&);
void selectPad(int index);
void triggerPad(int index, float velocity, bool noteOn = true);
void stopAll();
void autoSlice(slicelab::SliceMode mode, int count, float sensitivity);
void addSlice(double sourceSeconds);
void deletePad(int index);
void duplicatePad(int index);
void moveBoundary(int index, double startSeconds, double endSeconds);
void separateStems();
void cancelWork();
void undo();
void redo();
void savePreset(const juce::File&);
void loadPreset(const juce::File&);
void exportPad(int index, const juce::File&);
void exportMidi(const juce::File&);
void collectSample(const juce::File& directory);
void setBeatAnchors(const std::vector<slicelab::BeatAnchor>&);
void setViewState(const slicelab::ViewState&);
void beginEditGesture();
void endEditGesture();
```

Use these names and the domain structures unchanged. Additional private helpers are allowed. `setGlobal` and `setPad` must notify corresponding automatable host parameters. Host parameters include master gain, target BPM, sync, global tune, stem levels and enable states, and the audible per-pad controls. Automation and project restoration use the same state rules as GUI edits.

The additive full-scope fields include explicit Repitch mode, velocity sensitivity, snap, keyboard glide time, key override, independent stem solos, source beat anchors, a stored random seed, and editor view state. A solo does not overwrite prior enable states. Piecewise beat anchors correct source drift. The editor batches a multi-pad or drag edit between `beginEditGesture` and `endEditGesture` so undo restores one user action.

## Separation CLI

The installed worker is `~/.local/share/slicelab/stem_worker.py`. Source is repository-root `scripts/stem_worker.py`. An environment/configured override is allowed. Invoke using an argv array with `python3`, never a shell string.

```text
stem_worker.py separate --input /absolute/source.wav --result /absolute/result.json --status /absolute/status.json --host wojtek-ai-pc --quality best
```

Atomic status JSON has `state`, `detail`, and nullable `progress`. Result JSON has `schema`, `input_sha256`, `model`, `model_sha256`, `sample_rate`, `frames`, and a `stems` object with absolute `vocals`, `drums`, `bass`, and `other` WAV paths. C++ validates and decodes all four before replacing its asset. The original is the exact all-stems-on, unity-level path. All disabled means silence. Other combinations sum enabled estimates with their levels. Stem changes must be smoothed.

## Gates

Build the real VST3, then drive the real processor with sample-offset MIDI and the supplied sample. Verify independent tempo/pitch, trigger/hold, mono/poly, reverse, loop, envelopes, filters, per-pad edits, state roundtrip, output routing, and the CLI's actual four-stem result. Final REAPER verification includes real MIDI rendering, host tempo, save/reload, all-off silence, stem solo, window resize, and direct GUI manipulation. Every remaining acceptance gap stays visible.
