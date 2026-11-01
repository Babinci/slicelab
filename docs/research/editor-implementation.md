# Native editor implementation

The editor is a JUCE component backed by `SliceLabAudioProcessor::snapshot()`. It owns no decoding, separation, or audio jobs. The 30 Hz timer reads immutable sample data and current controls. File choosers and the beat-grid dialog are asynchronous. The default size is 1240 x 820 logical pixels and the resize limits are 960 x 680 to 2200 x 1500. Size, waveform viewport, selection mask, and follow preference are written to `ViewState`.

## Controls

- Open and drop accept source files. The File menu exposes relink, preset save/load, selected-pad WAV export, MIDI export, and asset collection. Undo, redo, and panic are in the header.
- Source BPM, source half/double and tap are separate from target BPM, target half/double and host sync. Global pitch, cents, source key override, master gain, trigger/hold, mono/poly, velocity, keyboard mode, root, glide, Stretch/Repitch, snapping, offset, beat-grid editing, source audition, and follow are visible.
- The overview always spans the source. The detail view uses original source seconds. It draws beat/bar and target-time marks, source-time marks, transient ticks, slice regions, selected start/end handles, cursor, and playhead. Drag the overview to pan. Wheel over either waveform to zoom. Click the detail to select a region and place the cursor. Double-click splits at the cursor. Drag a handle to trim or Shift-drag inside the selected region to move it. Alt temporarily bypasses snap.
- Transients, grid, equal, random, and suggest modes use the processor's `autoSlice` command with count and sensitivity. Add splits at the cursor. Duplicate uses the first empty slot. The octave duplicate is disabled when an empty slot or a full octave of pitch range is unavailable. Favorites protect pads during regeneration.
- The eight-by-four pad matrix shows stable pad numbers and MIDI notes 36 through 67. Click auditions a pad. Shift selects a range and Ctrl toggles members. The inspector edits the selection in a grouped undo gesture. It exposes name, favorite, start/end, gain, pan, semitones, cents, time, filter, resonance, attack, release, reverse, loop, and stereo output.
- Stem gain, mute, and solo are separate. Mute does not clear solo or vice versa. Stem controls remain disabled until validated stems are ready. The status line reports background status, progress, errors, and missing readiness. Separate retries after an error and Cancel requests cancellation.
- Computer keys `12345678`, `qwertyui`, `asdfghjk`, and `zxcvbnm,` trigger pads 1 to 32. In keyboard mode they send consecutive MIDI notes from the configured root, up to MIDI note 127. Escape stops voices and source preview. Space toggles source preview. Ctrl+O opens audio, Ctrl+Z undoes, and Ctrl+Shift+Z or Ctrl+Y redoes. Text entry retains keyboard input and invalid numeric text is rejected. Losing editor focus releases computer-key notes.
- Grid Edit accepts increasing `source seconds, beat` pairs. Two or more anchors define a piecewise grid. The offset shifts the anchor-derived grid. An empty list uses the corrected source BPM and offset.

Slider drags and multi-pad changes group processor undo state, and automatable fields receive paired host gestures. Numerical fields suppress programmatic updates while actively dragged or edited. Resulting key uses the selected pad's tuning and the engine's current stretch/repitch rate calculation. Source key or BPM that the analyzer could not detect remains visibly unknown.

## Verification and remaining limits

The initial editor object compiled with the repository's `scripts/env.sh` environment. A linked build and the real REAPER window remain the authority for visual and interaction checks. This document is not evidence that host automation, resize persistence, audio playback, separation, or export has passed an end-to-end test.
