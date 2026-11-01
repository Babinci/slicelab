# SliceLab editor

Use a resizable native JUCE editor with a default size near 1240 by 820 and a usable minimum near 960 by 680. Header commands expose sample loading, presets, undo, redo, and media export. The source summary shows filename, sample duration, detected key, key confidence, source BPM, and output BPM.

The top half gives the source waveform most of the width. It has an overview navigator, a zoomed detail view, beat and bar lines, transient marks, colored slice regions, draggable start and end handles, and a playhead. Wheel zooms around the cursor. Drag pans. A deliberate gesture adds a slice at a source position. Controls expose beat-grid offset and slice methods.

The lower half contains a 4 by 8 pad matrix, four stem strips with levels and enable controls, and a selected-pad inspector. Every pad shows its number, MIDI note, favorite state, and activity. The inspector exposes gain, pan, pitch, time, envelope, filter, reverse, loop, and output.

Job status, failures, missing media, and separation readiness are visible in the editor. Keyboard commands and tooltips describe gestures. Controls must retain usable hit targets at the minimum size.
