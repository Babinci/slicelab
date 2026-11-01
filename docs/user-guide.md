# Use SliceLab in REAPER

Insert **VST3i: SliceLab (SliceLab)** on a track. Send MIDI notes 36–67 to pads 1–32. The plugin has a stereo main output and 31 optional stereo auxiliary outputs. REAPER must enable and route an auxiliary bus before that output can be heard. A pad assigned to a disabled bus falls back to the main output.

## Load and slice

Click **OPEN**, press Ctrl+O, or drop an audio file onto the editor. WAV, AIFF, FLAC, Ogg, and MP3 are supported. The overview spans the complete recording; the large waveform shows the selected source interval. The source remains in its original time coordinates while the target-time ruler follows the tempo setting.

Choose Transients, Grid, Equal, Random, or Suggest, set the count, and click **SLICE**. Suggest ranks prominent audio events. Grid uses the selected interval from 1/16 through 4 beats. Favorite pads survive regeneration. Random slicing stores its seed in the session.

Click the waveform to place the source cursor. Double-click or use **+ ADD** to split there. Drag the selected start or end handle to trim. Shift-drag inside a selected region to move it. Alt temporarily bypasses snapping. The inspector also accepts precise start and end times. One boundary drag is one undo step.

Wheel over a waveform to zoom around the pointer. Drag the overview to move the detailed view. **FOLLOW** follows playback. **PLAY** auditions the complete source from the cursor; **STOP** stops source audition. Pad and source audition use the current stem and global transform settings. **METRO** adds a quiet, bar-accented click to source audition using the source beat map. It is off by default, follows tempo and offset edits, and does not affect pad playback or exported audio.

## Tempo and key

Source BPM describes the recording; target BPM describes playback. The source /2 and x2 buttons correct half-time or double-time interpretation. The target /2 and x2 buttons change playback tempo with one click. **SYNC** follows REAPER's tempo; its half/double buttons then change the host multiplier. **TAP** estimates source tempo from repeated clicks.

**STRETCH** changes time and pitch independently. Click it to select **REPITCH**, which couples them like changing playback speed. Global and pad tuning combine. Keyboard mode adds the played interval relative to its root note. The key shown for a selected pad includes its current tuning and repitch setting.

Use the source-key menu to correct the analyzer's key. The target-key menu transposes global pitch to the nearest matching tonic while preserving major or minor mode. An unknown key disables target-key selection. A detected key is an estimate; override it when needed.

**GRID EDIT** accepts increasing pairs of `source seconds, beat`. Two or more anchors define a piecewise beat map for recordings that drift in tempo. The offset shifts this map. An empty anchor list uses constant source BPM.

## Import cue positions

Choose **FILE → Import markers** to load CSV or TSV containing source seconds and an optional name:

```csv
seconds,name
0.000,Intro
3.250,First hit
8.500,Hook
```

A header is optional. Positions must be increasing, distinct, and within the recording; UTF-8 names and quoted CSV fields are supported. Imported slices end at the next position, with the last ending at the source end. Favorite pads are protected. The complete import is one undo step; invalid files or insufficient free slots leave the pads unchanged. This is text interchange rather than a native Serato DJ cue-file reader.

## Play and edit pads

Click a pad to audition it. Shift selects a range; Ctrl toggles individual selections. Inspector changes apply to selected pads as one gesture. **DUP** copies to the first empty slot; **+12 DUP** copies one octave higher. **DELETE** clears the selected pad. Pads expose level, pan, tuning, speed, filter, resonance, attack, release, reverse, loop, name, favorite, and output.

**HOLD** releases a voice when its note is released. With Hold off, a pad plays as a one-shot. **MONO** chokes earlier pad voices; with Mono off, pads overlap. **VELOCITY** enables velocity response. **KEYBOARD** plays the selected slice chromatically from the root note; mono keyboard mode supports glide. Source preview remains separate from pad voices.

The computer keyboard also plays the pad grid:

| Row | Keys | Pads |
| --- | --- | --- |
| 1 | `12345678` | 1–8 |
| 2 | `qwertyui` | 9–16 |
| 3 | `asdfghjk` | 17–24 |
| 4 | `zxcvbnm,` | 25–32 |

In Keyboard mode these keys play consecutive notes from the configured root. Text fields retain typed input. Losing editor focus releases computer-key notes. Escape or **PANIC** stops voices; Space toggles source audition. Ctrl+Z undoes; Ctrl+Shift+Z or Ctrl+Y redoes.

## Separate and mix stems

Click **SEPARATE** to prepare vocals, drums, bass, and other on the configured SSH worker. This installation uses `wojtek-ai-pc`. The plugin shows progress and enables the stem controls only after all returned audio and checksums pass validation. **CANCEL** requests cancellation; Separate can retry a failed job.

Each stem has a level, **M** mute, and **S** solo. Solo preserves mute choices. With every stem enabled at unity, playback uses the original recording. Other combinations use the aligned separated estimates. All stems off produces silence. Cached stems remain usable without the remote machine. For compressed audio and multichannel files, SliceLab sends a verified float WAV of the exact PCM it loaded so decoder padding cannot shift the returned stems. The source and transport hashes are recorded separately.

The current recipe is a pinned four-source BS-RoFormer checkpoint, full precision, overlap 8, and three-pass augmentation. Separation is an estimate and can contain bleed or missing detail. This project does not claim perfect separation or a measured winner over every available model.

## Save, export, and move a session

Save the REAPER project to preserve controls, pad boundaries, beat anchors, stem references, and editor size. **FILE** also provides preset save/load, selected-pad WAV export, MIDI export, asset collection, and relink.

Use asset collection before moving a project. Keep its collected source and stems together with the saved preset or project. Relink checks source identity so that an unrelated file cannot silently inherit another recording's slices. Missing media is reported and can be relinked. Presets and REAPER projects store media references rather than embedding the recording.

Undo/redo retain up to 64 edits with a 256 MiB budget for unique historical PCM. The current source is limited to 128 MiB of decoded stereo float PCM; source plus four stems is limited to 256 MiB. A rejected import leaves the current source available.

## Latency and practical polyphony

SliceLab reports an exact 20 ms processing delay to REAPER in Stretch and Repitch modes. REAPER compensates recorded and rendered tracks; live MIDI monitoring still includes this delay plus the audio device buffer. The plugin displays an overload counter when processing cannot meet its budget. An overload silences and resets queued voices rather than replaying stale audio.

Start with a 512-frame audio buffer on this laptop. Independent R3 stretching costs substantially more than neutral playback or Repitch. The measured machine does not sustain 32 simultaneous stretched voices live; all 32 passed offline rendering. Freeze or render dense stretched parts, reduce overlapping voices, or use Repitch when appropriate. A 128-frame buffer is not a verified reliable setting on this installation. See the [actual REAPER results](research/reaper-render-results.md) for measured settings and remaining limits.

## Optional analysis and worker configuration

Native BPM and chroma analysis is included. If the separately installed S-KEY/Essentia analyzer exists at `~/.local/share/reaper-key-detection/analyze_key.py`, the background worker can refine key detection. No analyzer or model is downloaded automatically. Set `SLICELAB_KEY_ANALYZER=off` before launching REAPER to disable refinement. Compatible installations can use `SLICELAB_KEY_ANALYZER` and `SLICELAB_KEY_PYTHON` overrides.

The installed separation client is `~/.local/share/slicelab/stem_worker.py`. SSH authentication belongs to the system's SSH configuration. If authentication expires, sign in through `ssh wojtek-ai-pc` and retry. Completed local caches are under `~/.cache/slicelab/stems`. Neither SSH credentials nor model weights are embedded in presets or the VST3.
