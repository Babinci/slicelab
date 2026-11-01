# Source metronome and marker-file import

The **METRO** button enables a quiet click while the source preview plays. It
uses the source BPM or the piecewise source beat anchors and grid offset shown
in the waveform. The first beat of each four-beat group is accented. Playback
speed changes move the click with the preview; the click is not a separate
free-running clock. Starting exactly on a grid line sounds that beat, while
starting between lines waits for the next line. Stopping or restarting preview
also stops or restarts its click. The master gain controls both preview audio
and click. The metronome defaults to off, is a host-automatable parameter, and
does not add clicks to pad playback or pad WAV exports.

**File → Import markers…** accepts a UTF-8 CSV or TSV file with exactly two
columns: source time in seconds and a name. The `seconds,name` header is
optional. Names may be quoted; within a quoted field, `""` represents a
literal quote. A comma is allowed in a quoted CSV name, and names in TSV may
contain commas without quotes. For example:

```csv
seconds,name
0.00,Intro
1.25,"Snare, bright"
2.75,"Vocal ""yeah"""
```

Times must be finite, unique, at least zero, and strictly before the end of
the loaded source. Import sorts rows by time and rounds each to the nearest
source frame; times that round to the same frame or past the last frame are
rejected. A file may contain up to 32
markers, subject to the number of unfavorited pad slots; names may contain up
to 80 UTF-8 bytes. Empty names are permitted. Malformed records, multiline or
control-character names, oversized files, repeated times, and insufficient
unprotected slots reject the whole import. Existing favorite pads are kept.
Each imported slice ends at the next imported marker, or at the source end for
the last marker; the complete import is one undoable action.

This is a documented text interchange format, not a native Serato DJ metadata,
SLAB, controller, or hardware protocol implementation.
