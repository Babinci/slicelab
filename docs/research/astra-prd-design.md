# SliceLab product and engine design

Research date is 2026-11-01. This document defines the target and the decisions needed to implement it. It does not report a working build, measured audio quality, a successful GPU job, or a completed REAPER test. Those claims require the evidence in [the REAPER verification plan](reaper-verification-plan.md).

The user wants a complete open-source Linux VST3 instrument. The central workflow is to load a recording, understand its tempo and key, choose or edit slices, perform them through MIDI, change timing and pitch independently, and isolate useful stems inside the plugin. A project must reopen with the same sound. The original editor must remain usable when resized.

The private input is `/home/wojtek/Documents/ChatGPT/slicelab/sample_test.wav`. The requested separation machine is `wojtek-ai-pc`. Neither the input nor its derived audio belongs in the public repository, release bundle, public screenshots with private metadata, or public test artifacts. This design does not establish that the remote machine is reachable or has the needed GPU capacity.

## Reference inventory and scope

The current official download page identifies Sample 2.2.0. Its additions include stem levels, stem automation, mouse-wheel parameter edits, and SLAB control. The older manual pages remain useful, but some details lag the current product, including an automation list that still mentions cues 1 through 16. Do not use that stale detail to limit SliceLab to 16 pads. [Current release notes](https://serato.com/sample/downloads), [manual automation list](https://support.serato.com/hc/en-us/articles/115003945934-Miscellaneous).

The table records observable workflows. It does not claim access to Serato's implementation.

| Reference area | Verified reference behavior | SliceLab scope |
| --- | --- | --- |
| Import | File dialog, file drop, cancellable analysis, replacement, and missing-file location. [Loading a file](https://support.serato.com/hc/en-us/articles/115000489094-Loading-a-file) | Essential. Import and restore must share the same validated asset loader. Replacement remains undoable until committed. |
| Waveform | Whole-source overview, detailed waveform, scrubbing, zoom, and a fixed editing view during playback. [Source player](https://support.serato.com/hc/en-us/articles/115000489334-Source-Player) | Essential. Retain the whole-source overview while zooming. Show source time and effective beat position. |
| Analysis and transport | Original BPM and key, target BPM, host sync, half and double tempo, detune, velocity, and beat-grid snapping. [Overview](https://support.serato.com/hc/en-us/articles/115000487673-Overview) | Essential. Separate source analysis from playback targets and expose manual corrections. |
| Slice performance | 32 cues, editable starts and ends, MIDI triggering, hold and trigger modes, mono and poly voices, keyboard mode, multi-selection, and output routing. [Cues](https://support.serato.com/hc/en-us/articles/115000501614-Cues) | Essential for 32 playable slices and manual editing. All other listed mechanics remain in the full adaptable scope. |
| Automatic selection | Content-based suggestions, consecutive beat slices, random slices, pitched duplication, favorites that survive regeneration, and Serato DJ cue import. [Autoset](https://support.serato.com/hc/en-us/articles/115000489113-Autoset) | Essential transient and grid slicing. Include independent suggestion, randomization, pitched duplication, and favorite protection. See the interoperability boundary below. |
| Pad sound | Per-cue level, filter, attack, release, reverse, pitch, speed, and mono keyboard glide. [Cue parameters](https://support.serato.com/hc/en-us/articles/115000500413-Cue-parameters) | Include these controls. Stable per-pad automation must address the same slot after reload. |
| Beat correction | Movable beat and tempo anchors, grid offset, editable BPM, half and double source BPM, tap tempo, metronome, and reversible edits. [Beatgrids](https://support.serato.com/hc/en-us/articles/360002025835-Beatgrids) | Include source beat-map correction. A constant-BPM grid alone cannot correct a drifting recording. |
| Stem audition | Four parts, mute and solo, initiation cost, and visual feedback that follows the chosen audio. [Stems](https://support.serato.com/hc/en-us/articles/7645256113551-Stems) | Essential. Real remote inference, reusable local results, four in-plugin controls, progress, cancel, and recovery. |
| Session editing | Undo and redo, velocity, and host-visible parameters. [Miscellaneous](https://support.serato.com/hc/en-us/articles/115003945934-Miscellaneous) | Include edits, host automation, and durable state. Do not automate destructive operations as accidentally repeatable scalar values. |
| Current additions | Independent stem levels and stem on/off automation. [Sample 2.2.0](https://serato.com/sample/downloads) | Include levels plus mute and solo. Define their automation interaction explicitly. |
| Files | Serato documents WAV, AIFF, MP3, FLAC, and Ogg among its cross-platform inputs, with platform differences. [Supported formats](https://support.serato.com/hc/en-us/articles/115000380193-Supported-File-Formats-for-Serato-Sample) | Verify the actual Linux decoder build. WAV, AIFF, FLAC, Ogg, and MP3 are the adaptable target. Never advertise a format from a library flag alone. |

Essential means the user explicitly needs it for the core workflow. The remaining adaptable mechanics are additional completion requirements, not a hidden later version. An implementation may sequence them, but a full-completion claim requires the full matrix below. Source-specific hardware protocols and proprietary metadata need separate interoperability work. Generic MIDI input and host MIDI learn must work regardless.

Musical notation means a readable key name such as A minor, an alternate pitch-name convention, and explicit semitone and cent shifts. The original estimated key and the resulting key remain visible together. A confidence value and an Unknown state distinguish detection from user correction. This specification does not invent a claim of full score transcription from a mixed recording.

## Completion and acceptance matrix

Every row begins unverified. The test IDs point into the companion plan. A screenshot can prove a layout state, but cannot prove the sound, MIDI timing, or saved state.

| ID | Requirement and observable acceptance | Evidence |
| --- | --- | --- |
| P01 | REAPER scans the installed Linux VST3 as an instrument. A MIDI item and live MIDI produce audio with the editor closed. | R01, R03 |
| P02 | File chooser and filesystem drop load the entire private recording. Duration and channel count match decoded media. A failed import preserves the previous working session. | R02, R14 |
| P03 | The full-source overview always represents all frames. Detail zoom and pan preserve the selected slice. The effective beat ruler changes with output tempo and manual grid correction. | R02, R06, R12 |
| P04 | BPM and key analysis run asynchronously, expose uncertainty, and accept manual correction. Known synthetic tempo and tonal fixtures establish accuracy. The private mix receives a listening and analysis report rather than an invented ground truth. | R02, R06 |
| P05 | All 32 pad slots accept MIDI, show pad number and numeric MIDI note, and produce their assigned interval. Mapping and names survive state restore. | R03, R10 |
| P06 | Transient slicing responds to a documented sensitivity control. Grid slicing follows the source beat map and chosen interval. Both preserve protected pads and support undo. | R05 |
| P07 | Manual creation, start/end dragging, numeric edits, snapping, free edits, deletion, duplication, and multi-edit produce the intended source-frame intervals. Boundaries cannot invert or escape the source. | R05 |
| P08 | Free target tempo, host sync, half tempo, and double tempo change duration without changing pitch in Stretch mode. Source-BPM correction remains a separate edit. | R06, R07 |
| P09 | Global semitones, detune, target-key selection, pad tune, and keyboard mode change pitch without changing duration in Stretch mode. | R07 |
| P10 | Hold, Trigger, Mono, Poly, velocity, reverse, attack, release, filter, level, and pan work through MIDI and host automation. Repeated notes, sustain, all-notes-off, and stealing cannot leave stuck voices. | R03, R04, R09 |
| P11 | Grid anchor edits correct a deliberately drifting source. Playback follows the corrected map at a new target tempo. Grid edits are reversible and persist. | R06, R10 |
| P12 | Suggestions, seeded random slices, favorite protection, pitched-pad duplication, and mono glide work without overwriting protected edits. | R05, R07 |
| P13 | Separate submits the actual source to `wojtek-ai-pc`, returns real vocals, drums, bass, and other/instruments, and makes them usable in the plugin. The result names its model and recipe. | R08 |
| P14 | Stem mute, solo, and levels affect every relevant voice, preview, output bus, and render. A ready set works without the remote machine. Loading another source cannot attach a stale result. | R08, R09, R14 |
| P15 | The same saved project restores media, slices, analysis corrections, grid, tuning, output routing, stem state, and editor size after a fresh REAPER process starts. | R10 |
| P16 | Missing or changed media produces a relink state with preserved edits. Collecting project assets produces a relocatable package. Paths with spaces and Unicode work. | R11 |
| P17 | All host parameters have stable IDs and correctly paired gestures. Stem automation and parameter envelopes survive reopen and offline render. | R09, R10 |
| P18 | Each pad can route to an independent stereo destination. Disabled host buses never receive invalid writes. REAPER receives only the selected pad on each configured bus. | R09 |
| P19 | The native editor is usable at minimum, default, and large sizes, at tested scale factors, and after repeated open/close. Text, handles, focus, and hit regions stay aligned. | R12 |
| P20 | Decode, analysis, separation, state restore, and cache activity cannot block or allocate in the audio callback. No corrupt output, dropped notes, or crashes occur during the stress suite. | R13, R14 |
| P21 | Fast offline render after project open produces the same timing and settings as realtime playback. It cannot depend on opening the editor or waiting an arbitrary number of seconds. | R07, R10, R15 |
| P22 | Unavailable server, GPU memory exhaustion, cancelled jobs, bad stems, partial transfers, corrupt state, unsupported files, and full storage produce bounded failures with a recoverable session. | R14 |
| P23 | A new checkout can build and install the documented artifact from pinned sources. The release excludes private media and includes applicable source and license notices. | R16 |

## What stays original and what cannot be promised

SliceLab can implement independently designed sampling workflows. Serato's product names, logos, illustrations, UI artwork, Pitch 'n Time implementation, source-selection algorithm, and separation model are not implementation assets. Use original labels, artwork, and code. The product should describe measured behavior, not claim proprietary algorithm equivalence. Serato identifies the branded engines in its [product description](https://serato.com/sample), and its [legal page](https://serato.com/legal) points to separate product and third-party terms.

Serato DJ cue import is a specific interoperability feature. A generic marker-import contract can carry time, label, and color without guessing a proprietary format. If direct import is claimed, verify a documented or independently validated reader against real files. Native SLAB integration similarly requires a real protocol implementation and hardware evidence. Generic MIDI support is not evidence of that integration.

A universal guarantee of perfect separation is physically underdetermined. If a mix is `x = a + b`, then `x = (a + u) + (b - u)` for any signal `u`. The mixture alone cannot identify which source assignment was the original. This is a mathematical limit, not a reason to reduce the feature. The deliverable remains strong, real four-stem separation, measured on reference stems and auditioned on the private recording. Report residual bleed and artifacts honestly. Do not put a percentage such as 100% on the control.

The best current model is an empirical question. A vocal-only model is not a four-stem implementation, and four output tensors do not prove four useful stems. The MSST authors explicitly warn that certain fine-tuned HTDemucs checkpoints have only one useful target output. Their catalog also includes four-source SCNet and BS Roformer candidates. These establish candidates, not a verified winner on this machine or this recording. [Author-maintained pretrained model catalog](https://github.com/ZFTurbo/Music-Source-Separation-Training/blob/main/docs/pretrained_models.md).

Benchmark a strong four-source candidate, a source-specialist recipe where appropriate, and a known baseline with the same reference corpus and export policy. Use the resulting quality, GPU memory, elapsed-time, and licensing record to choose the production profile. Preserve an explicit model manifest and checkpoint digest. Do not silently substitute a weaker model on error or label a default checkpoint state of the art without the comparison.

Checkpoint rights require their own evidence. Demucs code has an MIT license, but its maintainer separately says its weights are outside that license and supplied for scientific purposes. That primary statement prevents an automatic assumption that the weights can be bundled with an unrestricted product. [Maintainer statement](https://github.com/facebookresearch/demucs/issues/327#issuecomment-1134828611). The build's JUCE license is also separate from APC's MIT wrapper. JUCE currently documents dual JUCE-license and AGPLv3 terms. [JUCE licensing FAQ](https://juce.com/get-juce/). Record the exact dependency and weight terms before distribution. This does not prevent the authorized local investigation or the implementation of the remote worker contract.

## Two end-to-end architecture candidates

The comparison includes [the local APC and Linux inspection](apc-architecture-context.md). That inspection reports framework and machine facts. The judgments below are design recommendations.

| Question | A. Local sampler with immutable assets | B. Prepared banks with a local render service |
| --- | --- | --- |
| Domain owner | One non-realtime session owner publishes immutable sample and edit snapshots. The plugin owns the realtime voices. | A separate local service owns source assets, prepared pad banks, analysis, and time/pitch processing. The plugin is a host adapter and event scheduler. |
| Import and analysis | A bounded local worker decodes PCM, builds waveform levels, analyzes tempo/key/onsets, and publishes only complete assets. | The plugin sends an import transaction to the service. The service publishes a versioned asset handle and display data. |
| Audio path | A preallocated 32-voice engine reads immutable PCM or cached renders. A bounded realtime independent-time/pitch path handles live changes and cache misses. | The service prepares common pad renders and supplies future audio blocks through shared memory. A fixed lookahead makes unexpected live events audible after a declared latency. |
| Tempo automation | The engine consumes host tempo and timed parameter events directly. Cached banks are optional accelerators. | The service receives timestamped events and renders before each output deadline. Bank-only operation cannot support arbitrary uncached live tempo changes. |
| GPU separation | A local job owner sends a content-addressed request to `wojtek-ai-pc`, validates returned stems, then publishes one complete local stem set. | The service owns the same remote job protocol and can share results across instances. |
| Offline rendering | The plugin owns a deterministic processing path with prepared assets. Host offline lifecycle may allow a different bounded preparation policy. | An explicit synchronous offline request-response mode is needed. A realtime-only ring buffer will underrun when REAPER renders faster than the service. |
| Failure behavior | An analysis or remote job fails without removing the current playable asset. A plugin DSP fault still affects the host process. | A service crash can be isolated from REAPER, but playback requires service recovery, cached local audio, or a duplicated fallback engine. |
| Main cost | Careful realtime lifetimes, stretcher startup, and CPU bounds per voice. | IPC, scheduling, service lifecycle, fixed latency, asset leases, offline negotiation, and duplicate fallback behavior. |

Recommend A. It directly fits a MIDI instrument and keeps playback independent of remote inference. Native JUCE is the recommended editor for this run. The inspected APC Visage adapter lacks several required input-forwarding paths, while WebView adds a Linux runtime and embedding boundary. Native JUCE is an explicit APC workflow extension, not an already-supported third template. Retain APC's pinned JUCE dependency and build conventions, and record the extension in project-owned configuration.

An asynchronous per-pad Rubber Band bank is useful, but is insufficient as the sole audio engine. A new host tempo may arrive at the next processing block, before a replacement bank exists. Replaying the old bank gives the wrong tempo. Resampling it changes pitch. Waiting for a worker breaks realtime safety. The same race appears when REAPER renders immediately after state restore. A bank-only prototype can prove static settings, but cannot pass P08, P17, or P21. This is a design rejection based on the timing contract.

Freeze the public model and processor interfaces before parallel implementation. Keep the stretcher behind a small audio renderer boundary so measured Rubber Band and Signalsmith candidates can be compared without changing the editor or saved state. Rubber Band documents variable input/output counts and explicit startup delay handling. Signalsmith also documents separate input and output latency. Neither library's existence proves correct sampler onset timing. [Rubber Band integration](https://breakfastquay.com/rubberband/integration.html), [Signalsmith Stretch](https://signalsmith-audio.co.uk/code/stretch/).

## Shared data and command contract

All ranges below are design requirements. Types use units in their names. A frame index always means a frame in the canonical source asset, not an output frame or a byte offset.

| Record | Required content and invariant |
| --- | --- |
| `SampleAsset` | Immutable canonical float PCM, source sample rate, channel layout, frame count, decoded-content hash, original-file hash, decoder version, source locations, waveform pyramid, and optional completed analysis. Every channel has the same frame count. No per-channel normalization. |
| `AnalysisResult` | Asset hash, algorithm version, BPM estimate and confidence, optional key estimate and confidence, onset frames, and beat anchors. Unknown is a value. User overrides are separate. A late result is accepted only for its asset generation. |
| `BeatMap` | Strictly increasing source-frame and musical-beat pairs. The inverse mapping is defined. Constant BPM is the two-anchor case. Moving a grid anchor changes this map, not the source PCM or slice coordinates. |
| `PadState[32]` | Stable slot ID, explicit numeric MIDI note, name, color token, favorite flag, optional `[startFrame, endFrame)` interval, level, pan, filter, attack, release, reverse, tune in cents, speed multiplier, output bus, and optional choke group. A populated interval obeys `0 <= start < end <= frameCount`. Empty is distinct from a zero-length interval. |
| `PerformanceState` | Pad or Keyboard mode, selected keyboard pad, keyboard root note, MIDI input policy, Hold or Trigger, Mono or Poly, velocity response, glide mode/time, sustain behavior, and the documented voice limit. UI selection never changes MIDI routing accidentally. |
| `GlobalTransform` | Stretch or Repitch mode, Free or Host timing, explicit free target BPM, host-tempo multiplier, source-BPM override, global semitones, detune cents, and target-key display preference. Never overload one BPM field with both input and output meanings. |
| `StemState` | Four fixed roles `vocals`, `drums`, `bass`, `other`, their levels, mute mask, solo mask, and an asset-generation reference. The UI may label `other` as Instruments, but serialization preserves the role. Any solo makes the effective set equal to the selected solos minus their mutes. Clearing solos restores the previous mute mask. |
| `StemSet` | The four immutable aligned PCM assets, common sample rate, channel layout, exact frame count, gain policy, model and checkpoint IDs, processing recipe, checksums, and input asset hash. The set is installed atomically. |
| `SessionState` | Schema version, revision, asset reference, 32 pads, analysis overrides, beat map, global and performance states, stem settings and references, routing, and view state. Separate durable edits from current voices, progress, and remote credentials. |
| `Voice` | Fixed-pool slot, note identity, source-generation lease, captured slice interval, source cursor, output progress, transform state, envelope, filter state, routing, and bounded scratch storage. Ended or stolen voices release their lease without freeing large assets on the audio thread. |
| `RenderKey` | Asset hash, stem-set identity and mix, source interval, beat-map revision, sample rate, tuning, timing, direction, algorithm/version, quality settings, and relevant envelope policy. A cached render is used only when every sound-affecting field matches. |
| `ViewState` | Editor dimensions, scale, waveform viewport, follow-playhead state, selected pad IDs, focus context, and display notation. Persist relevant preferences without treating pointer hover or active animation as session sound. |

The initial default pad map is numeric notes 36 through 67. Always display the MIDI number because host octave naming differs. Keyboard mode uses a declared root note, initially 60, and maps pitch relative to it. Do not infer MIDI numbers from displayed note labels.

The editor submits typed commands such as import, create slice, move boundary, apply grid, set pad property, set global property, randomize, undo, request separation, cancel separation, relink, and collect assets. Each command carries the session revision or asset generation it targets. One non-realtime owner validates boundaries, applies a transaction, creates an undo record, and publishes a snapshot. A drag is one undo transaction. Randomization stores its seed and resulting boundaries. Favorites protect regeneration, not deliberate user deletion.

The processor consumes fixed scalar parameters and prepared asset handles. It does not consume UI widgets or mutable trees. Stable host IDs include global timing/tuning controls, four stem controls, and slot-addressed pad parameters. A display selection is not an automation target. Host gestures pair begin/end correctly even if focus changes or the editor closes.

## Timing, pitch, and realtime invariants

For a constant source tempo, let source BPM be `Bs`, target BPM be `Bt`, and pad speed be `v`. The source-time speed is `r = (Bt / Bs) * v`. Output duration is source duration divided by `r`. A tuning offset of `s` semitones and `c` cents gives pitch factor `2^((s + c/100)/12)`. Keyboard pitch adds the played-note minus root-note offset. Source sample-rate conversion is a separate factor.

In Stretch mode, changing `r` preserves pitch and changing pitch preserves duration. In Repitch mode, rate and pitch are deliberately coupled and the displayed effective tempo/key must reflect that coupling. The UI cannot silently switch modes because a filename looks like a short drum sample. Half and double output tempo affect the output target or host multiplier. Half and double source-BPM correction affect the source beat map.

For a drifting recording, map output beats through the inverse `BeatMap` to source frames. Keep fractional progress in sufficient precision and apply a common mapping to every stem. Looping the host timeline, seeking, stopping, and changing tempo have explicit voice-reset or continuation rules. Host tempo availability is independent of transport-playing state. A missing tempo leaves a visible unsynchronized state with a saved user target, not a fabricated host BPM.

These invariants are release gates.

1. The audio callback performs no file or network I/O, process launch, allocation, deallocation of large assets, mutex wait, condition wait, UI call, or unbounded retry.
2. The voice pool, stretcher state, filters, event storage, and scratch memory are allocated before processing. Maximum block size and work bounds are explicit. Zero-sample and variable-size calls remain valid.
3. MIDI events split rendering at their sample offsets. Events at the same offset have a deterministic ordering. Note-off never releases an unrelated later voice. MIDI note-on with velocity zero follows note-off semantics.
4. A completed audio-block epoch is insufficient for reclamation by itself. An active voice can retain an older asset for many blocks. Retirement requires both a safe epoch and no active voice leases. The audio thread never runs the final large-object destructor.
5. Source replacement and stem completion publish a whole valid generation. New voices use the new generation. Existing voices either finish on their captured generation or follow a documented short crossfade. They never read a freed asset or a mixture of generations.
6. Independent stem levels use common timing and linked stereo processing. A mute does not erase the stem cache. Auditioning all generated stems may differ from the original mix. Keep an explicit Original audition state and record reconstruction residuals instead of hiding them with independent normalization.
7. Host automation and realtime telemetry use bounded exchanges. Coalesce display-only progress and stale analysis requests. Never coalesce away a MIDI note-off. Report a queue overflow through non-realtime diagnostics.
8. A stretcher's input and output latency, startup padding, and tail policy are implemented and measured. Actual uncompensated delay is reported to the host. Priming work is part of the worst-case MIDI burst test.
9. State serialization reads a coherent durable snapshot. It does not walk live voices or wait for a remote job. Loading state validates its schema and schedules asset recovery without corrupting the current playable state.
10. Editor lifetime does not own playback, media, jobs, parameter storage, or persistent state. Closing the editor cannot stop MIDI playback or discard a separation job.

VST3 documents sample-accurate processor events and explicitly forbids controller messaging during its process call. JUCE's `copyState()` and `replaceState()` use locks and are not realtime-safe. These rules must survive the chosen wrapper, not just the design document. [VST3 API overview](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical%2BDocumentation/API%2BDocumentation/Index.html), [JUCE state API](https://docs.juce.com/master/classjuce_1_1AudioProcessorValueTreeState.html).

## Remote separation and durable media

The job state is `NotRequested`, `Queued`, `Uploading`, `Separating`, `Downloading`, `Validating`, `Ready`, `Failed`, or `Cancelled`. Failure and cancellation retain the last valid source and stem set. Retrying the same input and recipe reuses verified work where possible. The request generation prevents a result for source A from attaching after source B is loaded.

The request carries protocol version, job ID, request generation, canonical input hash, input sample rate, channel layout, frame count, requested roles, model profile, and recipe version. The response carries the same identity plus model revision, checkpoint digest, inference revision, preprocessing/resampling policy, overlap/segment settings, gain policy, output metadata and checksums, duration, and diagnostic status. Remote paths are never treated as local paths.

Use the user's existing authenticated connection to `wojtek-ai-pc`. Launch no remote command through interpolated sample names. The worker receives explicit data arguments and private job paths. Transfers write temporary files, validate checksums and audio headers, and atomically publish a complete set. All four outputs must have common alignment, channel count, and exact source-length mapping. Export float PCM without independently normalizing stems. A common gain adjustment is allowed only when recorded and undone consistently.

Do not expose a cosmetic active stem button before usable audio exists. Keep the requested choice visible while the status says Preparing. Local source playback continues. When the set is ready, apply the requested mix with a defined fade. Cache readiness survives editor closure and host restart. A newly restored project that depends on missing stems must expose that fact and must not silently render the original mix as an isolated vocal.

Persist an asset identity, last known absolute location, project-relative location where supported, and collected-asset reference. Relinking verifies the identity before applying old frame boundaries. A same-name different file is not an automatic match. If the user explicitly replaces media, treat it as a new import with a documented edit policy. Collect source and required stems into a user-chosen project asset directory with atomic writes. Plugin state should reference that directory instead of embedding hundreds of megabytes in every host state chunk.

The cached state also records analysis versions and user corrections. Reanalysis may update estimates only when requested. It cannot silently overwrite a manual BPM, key, beat map, or slice edit when a project reopens.

## Original native visual direction

Use a dark, precise editing instrument with warm off-white typography and restrained copper emphasis. The waveform is the largest object. The pad grid and stems are directly available below it. Avoid a copy of Serato's colors, pad treatment, symbols, spacing, or header arrangement.

The proposed palette uses ink `#10151A`, raised panels `#192128`, fine rules `#2C3842`, primary text `#EEE9DF`, secondary text `#A9B5BC`, and focus copper `#F4A56B`. Stem accents use muted rose, gold, blue, and green with text and state icons. These are proposed colors, not measured accessibility results. Verify text contrast and selected/disabled states on the rendered editor. Use a bundled font with recorded redistribution terms, clear tabular numerals, and a fallback with matching metrics.

The default editor is 1180 by 760 logical pixels. The minimum is 960 by 640. A larger window gives the waveform more height and the inspector more spacing. It does not scale text to unreadable sizes or hide essential controls behind a second pad bank.

| Region | Layout and interaction |
| --- | --- |
| Header | File title at left with a persistent Load action. Original BPM/key sit under the title. Large output BPM and output key sit near the center, with Sync, half, double, and Stretch/Repitch controls adjacent. Master level and a clear output meter sit at right. |
| Stem strip | Four equal labeled channels with mute, solo, and compact level controls. Separate has a persistent job status and cancel/retry action. Original audition is visibly distinct from the four-stem mix. No icon-only guessing. |
| Waveform | A narrow whole-source overview sits above a detailed waveform. Both show all cue markers relevant to their view. The detail pane shows the selected interval with separate start and end handles, beat lines, source-time labels, and a subtle live cursor. Follow view can be disabled while editing. |
| Slice toolbar | Transients, Grid, Suggest, and Random are named modes. Sensitivity or beat interval appears next to the selected mode. Snap, Add at cursor, favorite protection, undo, and redo remain visible. Grid Edit opens a dedicated beat-anchor mode with Apply and Cancel. |
| Performance area | All 32 pads form an 8 by 4 grid. A pad shows its number, name, MIDI number, and a small interval graphic. A playing pad receives a short light edge and progress indicator. The selected pad has a stable outline. Selection and playback are distinguishable. |
| Pad inspector | The selected pad's start/end values, tune, speed, level, pan, filter, attack, release, reverse, output, favorite, and name appear in compact groups. Multiple selected pads show Mixed values with batch edits. Keyboard mode exposes root and glide. |
| Footer | Hold/Trigger, Mono/Poly, velocity, keyboard mode, and panic are always reachable. A quiet status field shows source readiness, audio-engine readiness, or a concrete recoverable error. |

At minimum width, shrink gaps and waveform decoration before shrinking text. Preserve a minimum 28-pixel hit region for small controls and wider invisible hit targets around slice handles. Numeric fields accept typed values, fine dragging, wheel changes, and an explicit reset. Parameter changes use the host gesture protocol. Tooltips report units and shortcuts, not implementation details.

A waveform click selects or moves the audition cursor. A dedicated Add action or double click creates a slice at the cursor. Dragging inside an existing region moves the region only with an explicit move affordance or modifier. Start/end handles edit their respective boundaries. Snap state is always visible, and a temporary modifier bypasses it. The overview drag pans the detail selection. Wheel zoom is centered on the pointer. Neither wheel zoom nor pad triggering steals the host's text-entry shortcuts while a numeric field is active.

The empty view explains that a file can be dropped or opened and retains recognizable controls in disabled form. Analysis displays progress without obscuring the waveform. A missing source shows its name, expected duration, and Relink. A separation failure names the failed stage and offers retry. The application must preserve the musician's edits across these states.

## Highest-risk decisions to settle with the actual plugin

The first decision is independent stretch latency and CPU cost under simultaneous note-on. Compare prepared banks plus a bounded live path against a live stretcher pool. Reject any path that misses first notes, changes pitch during a tempo change, or relies on the editor to become ready.

The second decision is media lifetime and state restore. A pointer swap that passes a short smoke test can still free data used by a long voice. A state chunk that restores controls can still render silence because asset loading races offline rendering. Both need deliberate adversarial tests.

The third decision is the remote quality profile. Choose the real model recipe with measured four-stem results and explicit checkpoint terms. A successful Python exit code, four WAV filenames, or attractive buttons do not prove separation.

The fourth decision is the minimum-size native layout in REAPER. The data and gestures are known. The final spacing, font metrics, click targets, resize behavior, and visual density need the real editor, not a web mockup.

Throughput checkpoint is not applicable to this read-only investigation. Model the Domain shaped the versioned session, asset, and job records. Experience First kept all 32 pads and editable analysis visible instead of reducing the requested scope. Exhaust the Design Space produced two distinct execution models. Prove It Works and Test Behavior, Not Implementation require actual REAPER renders, cold restores, and interaction evidence for completion.
