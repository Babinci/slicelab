# SliceLab

An open-source sampling instrument for Linux VST3 and standalone use. SliceLab combines a complete source waveform, 32 playable slices, independent tempo and pitch, editable beat maps, and four-stem separation on an SSH-connected GPU worker. Its graphite and copper editor is an original native JUCE design.

SliceLab is an independent implementation inspired by sampling workflows. It contains no Serato code, branding, artwork, or models. It is not affiliated with Serato.

## Build and install

The Linux adapter builds through [Audio Plugin Coder](https://github.com/Noizefield/audio-plugin-coder), pinned as a submodule, and its pinned JUCE dependency. The currently tested toolchain is x86-64 Ubuntu with GCC 13. Bootstrap requires Git, GCC/G++, pkg-config, Python 3, uv, apt-get, and dpkg-deb. It downloads development packages into this repository without changing system packages.

```bash
git clone https://github.com/Babinci/slicelab.git
cd slicelab
bash scripts/bootstrap-linux.sh
bash scripts/build.sh
bash scripts/install.sh
```

Restart REAPER or rescan VST3 plugins, then insert **VST3i: SliceLab (SliceLab)**. The installed bundle is `~/.vst3/SliceLab.vst3`. The standalone application is under `build/plugins/SliceLab/SliceLab_artefacts/Release/Standalone/`. Runtime dependencies include ALSA, X11, fontconfig, FreeType, FFTW3, and libsamplerate. See [the Linux build notes](docs/linux-build.md).

## Make music

1. Open or drop WAV, AIFF, FLAC, Ogg, or MP3 audio.
2. Choose Transients, Grid, Equal, Random, or Suggest and click **SLICE**. Double-click the waveform to add a slice; drag its handles to trim.
3. Play MIDI notes 36–67 or click the pad grid. Select **KEYBOARD** to play one slice chromatically.
4. Set the target tempo or enable host **SYNC**. Target /2 and x2 change tempo in one click. Source controls correct the recording's BPM interpretation.
5. Click **SEPARATE** to prepare vocals, drums, bass, and other. Mix, mute, or solo them inside the plugin.

Pads have tuning, speed, gain, pan, filter, envelopes, reverse, loop, favorite protection, names, and stereo output assignment. The editor supports zoom, pan, follow, multi-selection, grouped undo, resize persistence, source-key correction, target-key transposition, presets, media collection, WAV export, MIDI export, text-marker import, and a source-preview metronome. See [the user guide](docs/user-guide.md) for controls and shortcuts.

## Remote stems

The current installation uses the system SSH alias `wojtek-ai-pc`. The client and remote worker are ordinary Python scripts in `scripts/`. The pinned recipe uses BS-RoFormer, full precision, overlap 8, and three-pass augmentation. It verifies source identity, model/configuration hashes, output checksums, frame counts, channel counts, and processing gain before attaching results. Completed local caches work without the worker.

The repository does not contain model weights, credentials, or development recordings. The worker setup and actual model recipe are documented in [the separation report](docs/research/stem-implementation.md). An SSH alias alone does not install the remote inference environment. Separation can contain bleed and artifacts; no claim of perfect separation or a measured universal model winner is made.

Native BPM and key estimation is included. A separately installed compatible S-KEY/Essentia analyzer can refine key detection in the background. See [analysis and memory behavior](docs/research/feature-completion.md).

## Verification

Development uses independent synthetic fixtures, processor behavior tests, allocation probes, pluginval, actual REAPER renders, and live PipeWire JACK captures. The acceptance plan deliberately distinguishes planned checks from measured results. See [the testing handoff](docs/verification.md), [the REAPER results](docs/research/reaper-render-results.md), [the acceptance plan](docs/research/reaper-verification-plan.md), and [the decision trail](docs/decisions.tsv).

The installed R3 candidate passed 43/43 REAPER render jobs and 43/43 audio checks, including the highest 64-channel output route, and GUI-enabled pluginval strictness 5. Behavior suites cover slicing, state, independent tempo/pitch, source replacement, markers, preview clicks, and verified remote results. Final live and editor checks are tracked separately in the results report; baseline results do not certify later binaries.

The plugin has a fixed 20 ms reported delay. Start at a 512-frame device buffer on the tested laptop. This machine does not sustain 32 simultaneous R3-stretched voices live; dense stretched parts can be rendered offline, where all 32 voices passed. Consult the results report for artifact hashes, measured settings, and remaining limits.

```bash
uv pip install --python .toolchain/bin/python -r requirements-test.txt
bash scripts/test.sh
```

The private `sample_test.wav` is optional development media and is ignored by Git. Some integration checks require that file, the separately installed key analyzer, the remote worker, or a local REAPER installation; they must report their actual availability rather than silently count as verified.

## License

SliceLab is AGPL-3.0. APC is MIT-licensed; JUCE is used under its AGPL option. The pinned Rubber Band library is built with a small reset-buffer reuse change in a build-directory copy. Its GPLv3-compatible terms and the separate model grant are documented in [third-party notices](NOTICE.md).
