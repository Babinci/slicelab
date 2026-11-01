# Verify SliceLab in real REAPER

This plan is dated 2026-11-01. Every test below is planned and unverified until its artifacts exist. A passing compiler, plugin validator, unit test, screenshot, or worker self-report does not substitute for loading and exercising the installed VST3 in REAPER.

The product requirements and architecture are in [the SliceLab design](astra-prd-design.md). The matrix uses P01 through P23 from that document. The local framework investigation identifies `/home/wojtek/opt/REAPER/reaper` and a PipeWire launcher, but this plan does not claim that either has run the new plugin. [Local environment inspection](apc-architecture-context.md).

## Establish an evidence record before testing

1. Create a private verification run directory with a unique run ID. Keep the private recording and its renders outside published artifacts.
2. Record the git revision and dirty diff, build type, compiler, JUCE pin, DSP-library pin, plugin version, VST3 identity, bundle path, and binary hash.
3. Record the actual REAPER version, process executable, OS, CPU, audio backend, negotiated sample rate, buffer size, channel layout, and plugin-reported latency.
4. Record the remote machine identity, GPU model, available memory, driver/runtime versions, model profile, checkpoint digest, and recipe version for stem tests.
5. Create a manifest that maps each test ID to its setup, expected result, actual result, status, evidence paths, and remaining defect. Use Passed, Failed, Blocked, or Not run. Do not use Passed with a caveat that contradicts the requirement.
6. Save the actual `.RPP` project, MIDI input, relevant preset/state, rendered WAV files, numeric measurements, and screenshots needed to reproduce each claim. Record hashes for source and render files.
7. Keep one public-safe report with synthetic fixtures and measurements. Keep private-song audio, stems, metadata, full paths, and screenshots in the private run directory.

Use REAPER's supported ReaScript API for repeatable project setup, MIDI items, parameters, saves, and renders where it can control the same host behavior. Verify the generated project in the running host. Do not mark mouse gestures, native file drops, or actual resize behavior as tested by writing a state chunk. [Official ReaScript API](https://www.reaper.fm/sdk/reascript/reascripthelp).

## Prepare independent fixtures

Generate small deterministic audio and MIDI fixtures outside the plugin implementation. Derive expected results from their construction and the product contract. Do not derive expected output by calling the same helper that implements the feature.

| Fixture | Purpose |
| --- | --- |
| A stereo coded pulse with known leading silence and distinct left/right patterns | Identifies event timing, source offsets, channel swaps, truncation, and repeated-note errors by correlation. Put the recognizable pulse after the declared boundary fade so its absence cannot be excused by that fade. |
| A 120 BPM click track with a known first downbeat and irregular accent levels | Establishes BPM, phase, beat positions, grid slicing, and tempo-sync duration. Include a nonzero leading offset. |
| A click track with explicit tempo segments and a gradual drift | Establishes that editable beat anchors and playback warp correct timing beyond a single constant-BPM label. |
| Long tones and a short known major/minor progression | Establishes pitch-shift frequency, detune, keyboard intervals, key confidence, and duration independence. Include an unpitched noise file that should not receive a confident invented key. |
| Sparse transients with known onset positions and varying levels | Establishes transient sensitivity, minimum slice spacing, onset timing, and no out-of-range slices. |
| Four distinct synthetic component files with a known sum | Establishes stem transport, labeling, alignment, gain, mute, solo, routing, and cache behavior. These do not prove ML separation quality. |
| Rights-cleared natural multitracks with true vocals, drums, bass, and other stems | Establishes model quality, bleed, musical artifacts, and model comparisons. Preserve reference alignment and licensing records. |
| The user's private `sample_test.wav` | Establishes the actual requested creative workflow, natural-audio audition, source length, manual edits, and real remote processing. Its true constituent stems are unknown. |
| Short, empty, truncated, corrupted, unsupported, mono, and sample-rate-varied files | Establishes boundary validation and recovery. Include spaces, Unicode, and long but valid path names. |

Create MIDI cases with notes at offsets that differ by one frame, notes on both sides of a callback boundary, overlapping same notes, simultaneous chords, velocity zero, ordinary note-off, sustain, and all-notes-off. Include a 32-note burst, tempo transitions, host transport seeks, and host loop wrap.

Use a clean REAPER project with track and master processing disabled for numeric comparisons. Keep project play rate at unity, disable render normalization, and render float WAV. Save the exact render range, tail setting, sample rate, and channel mapping. A nonzero output meter is insufficient evidence.

## R01. Load the installed artifact

Requirements are P01 and P23.

1. Inspect the actual VST3 bundle and its shared-library dependencies. Confirm that it resolves through the intended local dependency path.
2. Scan the configured user VST3 directory in REAPER. Record the plugin identifier that REAPER loads.
3. Insert SliceLab on a new track. Save the project and reopen the editor twice.
4. Close the editor, send MIDI through a real REAPER MIDI item, and render the track after loading the timing fixture.
5. Start a fresh REAPER process and open the saved project.

Pass when REAPER recognizes an instrument, the intended binary loads, MIDI makes the expected audio with the editor closed, and the fresh process restores the instance. Capture the FX entry, the loaded editor, the process/binary identity, the project, and the rendered WAV. A standalone application pass does not satisfy this test.

## R02. Import, analyze, and navigate a complete source

Requirements are P02 through P04.

1. Load the private recording through the native chooser. Record decoded duration, sample rate, channels, and frame count from an independent reader.
2. Verify that the overview includes the first and last frames. Seek near the start, middle, and end. Audition identifiable events at each position.
3. Zoom and pan the main waveform while the overview remains visible. Disable follow view and play a MIDI sequence while editing a distant slice.
4. Repeat import through an actual filesystem drag and drop. Test REAPER media-item drag separately if the product claims it.
5. Load known-tempo, tonal, ambiguous, and silence fixtures. Record BPM/key estimates, confidence, and completion time.
6. Correct BPM and key manually. Reload the state and check that analysis does not overwrite either correction.

Pass when full media length is available, source and displayed positions agree, native import gestures work, and the UI reports uncertainty honestly. On the known steady click fixture, target BPM error is at most 0.1 BPM after phase is set. Half-time or double-time analysis ambiguity must be visible and correctable with one action. Do not score the private recording's key as correct without independent musical evidence.

Capture the full waveform, zoomed boundaries, displayed source and target values, source metadata, and analysis results. These are acceptance targets, not measurements already obtained.

## R03. Prove MIDI timing and pad mapping

Requirements are P01, P05, and P10.

1. Assign 32 distinct intervals or coded fixtures to all pad slots. Keep their numeric MIDI notes visible.
2. Render a sequence that plays every pad through a REAPER MIDI item. Compare each region with its expected source interval.
3. Render timing notes at output-frame positions 17, 63, 64, 127, 128, and 129, followed by a longer series that crosses many callback boundaries. Use separate non-overlapping intervals where needed.
4. Run the case at 48 kHz with 64-, 128-, and 512-frame audio settings. Include 44.1 kHz and a 96 kHz case. Record what the backend actually negotiated.
5. Render with the editor closed, then test a live hardware/controller note path and the plugin's clickable pads.
6. Send note-on velocity zero, repeated same-note note-ons, their note-offs, sustain on/off, and all-notes-off.

At neutral tuning and speed, correlation against the known coded pulse must show the expected onset within one output frame after documented PDC and source lead-in. Do not use a coarse first-nonzero threshold as the only check. A fixed block-sized error reveals ignored MIDI offsets. A one-time correct first note does not prove subsequent notes.

In shifted or stretched modes, measure onset alignment separately from the algorithm's transient shape. The declared alignment target is within 1 ms after compensation, with no accumulating drift. Preserve waveforms and correlation plots for review. Any larger discrepancy needs a resolved defect or an explicitly accepted product limit before a full-completion claim.

The actual live path also needs measured total latency. Report interface, buffer, and plugin contributions separately. PDC that aligns an offline render does not eliminate the delay a musician hears while playing.

## R04. Prove voice and envelope behavior

Requirements are P05 and P10.

1. In Hold mode, release a note during a long slice. Measure the release tail and verify that the voice ends.
2. In Trigger mode, release the MIDI note early. Verify playback to the documented end or envelope termination.
3. Play overlapping pads in Mono mode and in Poly mode. Verify the promised choke and overlap behavior.
4. Trigger more notes than the voice limit. Confirm deterministic stealing with a short fade and no stuck note.
5. Trigger duplicate note numbers before their note-offs. Ensure that an old note-off cannot incorrectly release an unrelated stolen/reused voice.
6. Automate level, pan, filter, attack, and release. Compare channel balance, envelope timing, and spectrum with the fixture's expected behavior.
7. Reverse a known asymmetric pulse pattern. Verify playback order, start/end boundaries, and the final tail.
8. Replace the source and edit a boundary while long voices play. Verify the declared captured-generation or crossfade behavior.

Pass when the rendered audio matches the mode contract, no interval reads outside its source, and stopped or stolen voices terminate. Check finite samples and unreasonable discontinuities. Do not treat every source transient as a click defect. Compare transition artifacts against the same source and a known fade.

## R05. Prove slicing and reversible edits

Requirements are P06, P07, and P12.

1. Run transient slicing on the sparse-onset fixture at several sensitivity values. Compare detected boundaries with known onset positions and document the onset tolerance.
2. Run grid slicing at 1/16, 1/4, 1, and 4 beats from a known offset. Verify the frame intervals through both UI values and the resulting audio.
3. Favorite several nonadjacent pads. Regenerate transients, suggestions, and random slices. Confirm that their boundaries and parameters remain unchanged.
4. Repeat seeded randomization after state reload. Confirm reproducible boundaries. A new randomization must change unprotected eligible pads.
5. Create a slice manually, drag each boundary, enter frame/time values, disable snap, and apply a temporary snap bypass.
6. Drag a boundary beyond its neighbor and beyond the source ends. Confirm valid clamping or rejection without a crash or a hidden zero-length pad.
7. Select multiple pads, edit a shared property, duplicate a pad, rename it, delete it, and undo/redo the sequence.
8. Drag one boundary continuously and undo once. Verify that one drag creates one undo transaction.
9. Repeat slice editing near the final source frame and on a very short file.

Pass when audible intervals agree with displayed intervals and persisted values. Grid-slice frame positions must agree with the independent beat-to-frame calculation within one source frame. Transient tolerance is declared before evaluation, initially 10 ms for the synthetic pulse fixture. Manual edits remain the exact canonical source-frame positions across output-tempo changes.

## R06. Prove tempo, beat maps, and sync

Requirements are P03, P04, P08, and P11.

1. Use the 120 BPM fixture with a known source-grid offset. In free Stretch mode, render at 90, 120, and 150 BPM.
2. Click half and double tempo. Verify both the display and the rendered duration. Repeat under Host sync and verify the host multiplier.
3. Change the source-BPM interpretation from 120 to 60 and back. Confirm that the operation edits interpretation rather than silently changing the stored target.
4. Add a REAPER tempo change from 90 to 140 BPM inside a sustained slice. Include a smooth tempo ramp if the supported host information makes it available.
5. Repeat the tempo transition immediately after a note-on, during a 32-note burst, and during an offline render begun immediately after reopen.
6. Edit anchors on the drifting fixture. Render against the host click and compare every known beat, not only the first and last.
7. Seek transport, loop a region, stop, restart, and vary host play rate. Verify the documented synchronization behavior.
8. Exercise missing or invalid host-tempo data through the processor boundary test. Verify the visible unsynchronized/user-target state in the host case where available.

For a constant source, independently calculate expected duration as `sourceDuration * sourceBpm / targetBpm / padSpeed`. Verify exact scheduled end-frame accounting, allowing one output-frame rounding difference. Audible tails follow the documented envelope/tail rule and are measured separately. The pitch of the tone component must remain within 5 cents of its original value in Stretch mode.

For the beat-map fixture, the design target is no more than 2 ms beat drift across 64 bars after correction, excluding a separately documented transient-shape tolerance. Record per-beat errors so a late correction cannot hide earlier drift. A BPM label that follows REAPER without corresponding audio changes fails.

## R07. Prove pitch, keyboard mode, and stretch independence

Requirements are P08, P09, P12, and P21.

1. Render a long tone at zero shift, plus and minus 12 semitones, plus 7 semitones, and plus/minus 50 cents.
2. Measure the fundamental on a steady region against `f * 2^((semitones + cents/100)/12)`. Use an independent estimator with enough window length and interpolation.
3. Repeat at several target tempos. Verify pitch and duration as separate quantities.
4. Give two pads different tune and speed settings. Render a sequence and a chord that exercise both simultaneously.
5. Enable Keyboard mode. Play the root, octave, and chromatic intervals through the REAPER piano roll. Verify the displayed root and numeric MIDI mapping.
6. Enable Mono glide. Play overlapping notes in each supported retrigger/sustain mode. Confirm the pitch path and envelope behavior. Verify that Poly mode reports the defined glide behavior.
7. Switch to Repitch mode. Confirm the deliberate pitch/time coupling and accurate resulting displays. Switch back and confirm independent behavior.
8. Repeat the tested settings on drums, bass, vocals, and a dense private-song section. Audition attack clarity, stereo image, modulation, ringing, and pitch artifacts.

The numeric pitch target is within 5 cents for steady test tones. Constant-ratio duration accounting must satisfy R06. Audible quality also requires listening to the real renders at normal and extreme settings. Correct frequency on a sine does not prove acceptable drum transients or vocal quality.

Include algorithm version, quality settings, actual latency, voice count, and processing load with each comparison. A prepared bank can pass static cases, but it must also pass live tempo changes and immediate offline rendering to qualify as the production path.

## R08. Prove real remote stems and local isolation

Requirements are P13 and P14.

1. Submit the actual private source through the in-plugin Separate action. Record its input hash and the remote job ID.
2. Verify that the requested model runs on the GPU on `wojtek-ai-pc`. Preserve the worker manifest and GPU/job evidence. A job that silently ran a different model or only copied the input fails.
3. Validate each returned stem's role, hash, sample rate, channels, frame count, gain policy, and alignment metadata before it becomes ready.
4. Audition vocals, drums, bass, and other independently. Confirm that each is a real separated estimate. Four duplicate mixtures, silent placeholders, or non-target dummy outputs fail.
5. Render the plugin at neutral transform with each stem soloed. Compare it against the corresponding delivered PCM after the plugin's declared envelope and timing behavior.
6. Render all stems, every single mute, and selected combinations. Verify the effective masks and gain arithmetic. At neutral transform, compare the plugin's all-stem render against the sum of the delivered stems.
7. Toggle mute/solo during a sustained slice and during preview playback. Check fade behavior and that all active voices follow the chosen policy.
8. Save and restart REAPER with the remote machine unavailable. Verify that completed local stems still work.
9. Start source A separation, load source B, and let A complete. Confirm that A's result remains associated with A and cannot attach to B.
10. Cancel during upload, inference, download, and validation. Retry and verify idempotent reuse or safe replacement of partial work.

For routing and transport, the design numeric target at neutral transform is RMS error below `1e-6` and peak error below `1e-5` against the delivered float PCM after accounting for the known envelope. Measure left and right independently. All-muted output must reach digital silence after the declared fade/tail, with no continuing voice leak.

For model quality, run the same pinned candidates on rights-cleared multitracks with reference stems. Report per-stem SDR or SI-SDR with the exact evaluator and aggregation, plus scale/gain error, alignment, runtime, GPU memory, and a short artifact review. These metrics answer different questions. A near-perfect sum-to-mixture residual alone cannot prove source separation.

Select the strongest eligible measured recipe, with no unexplained material regression on one stem hidden by the average. The reference plan flags a median per-stem regression exceeding 0.5 dB for investigation, rather than inventing a universal pass threshold. Listen to held-out excerpts with dense overlap and to the private source. Record leakage, missing content, transient damage, and stereo artifacts. An unmeasured assertion of state-of-the-art or 100% separation fails the claim, even if the job finishes.

## R09. Prove automation and output routing

Requirements are P10, P14, P17, and P18.

1. Enumerate the actual host-visible parameters and IDs. Compare them with the public contract.
2. Write REAPER envelopes for global BPM/key, pad tune/speed/filter/level, and all four stem levels and on/off states.
3. Record a UI gesture into host automation. Verify both beginning and ending the gesture. Repeat with a numeric field, wheel edit, lost focus, and editor closure.
4. Render automation transitions near MIDI boundaries and during a held note. Compare the event timeline with the resulting audio.
5. Save and reopen the project. Verify the same envelope controls the same parameter.
6. Route separate pads to different stereo buses, including the highest supported output. Build the corresponding REAPER track routing and render multichannel or separate files.
7. Disable unused buses and repeat playback. Confirm no output on unintended channels, memory errors, or loss of the main output.
8. Solo and mute stems while pads use different outputs. Verify consistent behavior on every bus.

Pass when automation changes the audible property it names, IDs remain stable, gestures close, and output buses contain only their assigned signals. Cross-channel leakage must be at the documented noise floor for digital routing, ideally exact silence where no voice is assigned. Do not infer routing from labels or meters alone.

VST3 supports sample-accurate parameter transmission, but a framework parameter adapter can reduce that precision. Record the actual tested automation resolution. If the chosen JUCE path supplies block-rate values, disclose and test that behavior instead of claiming sample-accurate automation by reference to the VST3 specification. MIDI sample offsets remain a separate requirement.

## R10. Prove cold state restore

Requirements are P05, P11, P15, P17, and P21.

1. Create a non-default session with manually edited slices, two favorites, names, source-BPM/key overrides, a corrected beat map, pad transforms, output routing, host sync, a stem mix, and a resized editor.
2. Render a deterministic MIDI sequence and save the project.
3. Quit the REAPER process completely. Start a fresh process and open the project with the editor initially closed.
4. Render immediately. Do not insert a sleep or open the plugin window as a readiness workaround.
5. Compare the output and visible settings with the pre-save session. Use exact or declared-tolerance comparisons appropriate to the deterministic DSP path.
6. Repeat with two independent SliceLab instances using different samples and overlapping remote jobs.
7. Save while analysis or separation is in progress. Reopen and verify a recoverable pending/not-ready state with the correct edits and source identity.
8. Load an older supported schema fixture and an unsupported future/corrupt schema. Confirm migration or a safe bounded error without discarding the current working session.
9. Save a host preset and reload it in a new track. Verify the defined media-reference behavior.

Pass when the fresh host process renders the intended session without UI-dependent initialization, missing first notes, or stale transforms. Parameter equality alone is insufficient. Persisted editor size is checked visually. Ongoing voices and ephemeral progress need not resume as if the host never stopped, but durable musical choices must.

## R11. Prove relink and project portability

Requirements are P02, P15, and P16.

1. Copy the source to a private test directory, create a project using that copy, save, and close REAPER.
2. Move the test copy. Reopen the project and inspect the missing-media state.
3. Verify that all slices, grid edits, names, and transforms remain available for recovery.
4. Relink the identical file from its new location. Verify content identity and reproduce the reference render.
5. Offer a different file with the same basename. Verify mismatch handling without automatic acceptance of old frame boundaries.
6. Use Collect assets to create a portable project directory with the required source and stems.
7. Copy the portable directory to a different path, temporarily make the original locations unavailable, and reopen it.
8. Repeat with Unicode paths, spaces, read-only source media, and a missing stem cache.

Pass when collected projects restore from their own assets and ordinary relinking restores the exact sound. A project that works only because a hidden global cache or original absolute path still exists fails portability. Keep these operations on private copies so the original `sample_test.wav` stays intact.

## R12. Verify the real native editor

Requirements are P03, P05, P07, and P19.

1. Capture the editor hosted inside REAPER at 960 by 640, 1180 by 760, and a larger supported size.
2. Test actual host-window dragging and the plugin's size controls if present. Reopen the editor and verify saved dimensions.
3. Test supported display scales, including 100%, 150%, and 200% where the desktop permits them. Verify actual logical/pixel sizing, not only a numeric preference.
4. Inspect the full waveform, overview, start/end handles, 32-pad grid, selected state, active voices, inspector, stem controls, meters, and errors at each size.
5. Click the smallest visible controls and drag slice handles near waveform edges. Verify hit regions match the visual positions.
6. Use wheel zoom, wheel parameter changes, fine modifiers, typed values, reset, keyboard focus, tab navigation, and Escape from edits.
7. Trigger computer-keyboard pads where supported. Confirm that entering a number or file path cannot accidentally trigger notes or steal host shortcuts.
8. Open and close the editor repeatedly while MIDI and remote progress continue. Move the window while audio plays.
9. Inspect empty, analyzing, ready, preparing-stems, failed, missing-source, multi-selected-pad, and keyboard-mode states.

Pass when every required action remains reachable, no text or control overlaps, no labels are clipped, focus is visible, and selected/playing/muted states are distinguishable without color alone. Measure text contrast from the actual palette. Capture each required state from the real plugin and identify the corresponding binary/run. A browser mockup or generated screenshot does not satisfy this gate.

## R13. Verify realtime safety and sustained use

Requirements are P10 and P20.

1. Exercise the callback under an allocation/deallocation detector or an equivalent instrumented build. Assert no callback file/network access, blocking locks, unbounded loops, or large object destruction.
2. Run 32 simultaneous notes at several pitch and speed settings. Include a burst that starts all stretchers together.
3. During playback, load another source, request separation, edit boundaries, automate tempo and stem levels, resize, save, and close/reopen the editor.
4. Run several plugin instances with different assets. Observe CPU, memory, audio underruns, and the UI response.
5. Run a 30-minute mixed-use soak at the target production configuration. Record warm-up and steady-state memory separately.
6. Repeat the short burst suite at low-latency and large-buffer settings. Exercise variable callback lengths and zero-length calls in the processor boundary test.
7. Run address/undefined-behavior checks separately from timing measurements. Instrumentation overhead must not be presented as release performance.

The primary target is 48 kHz and 128 frames on the actual workstation, with all 32 voices available. The release record must state which combinations pass. The aspirational timing budget is a 99.9th-percentile callback cost below 70% of the buffer interval, with no observed underrun or missed deadline in the bounded run. This is a target, not a measured fact or a universal hardware guarantee.

Measure cold note-start cost and sustained processing separately. Average CPU can hide a first-note deadline miss. A 128-frame buffer at 48 kHz lasts about 2.67 ms by arithmetic. That is not a claim that the plugin has 2.67 ms of exclusive CPU time. If the full voice target fails, optimize or resolve the limit explicitly before claiming the requested full implementation.

Memory should plateau after configured caches warm. Repeated source replacement and old long voices must eventually release retired assets. Inspect actual memory ownership or sanitizer findings in addition to process RSS, which may retain allocator arenas.

## R14. Prove failure recovery and stale-result rejection

Requirements are P02, P14, P20, and P22.

Run these failures against a private test project with a known-good session already loaded.

| Failure | Required observable result |
| --- | --- |
| Unsupported, empty, corrupted, or truncated input | A bounded error appears. Existing playback and edits survive. The plugin does not publish a partial asset. |
| Remote host unavailable or authentication fails | Separation reports the failed stage and allows retry. Playback of the source and cached stems continues. |
| GPU memory exhaustion or model initialization failure | The worker returns a specific failure. The plugin does not report Ready or silently use another model. |
| Cancel during each job stage | The UI reaches Cancelled or a completed race outcome consistently. No cancelled partial set becomes active. |
| Transfer interruption or one damaged/missing stem | Validation fails the whole set. The old valid stem set remains usable. |
| Source changes while a job runs | The old job cannot replace the new source's stems or analysis. |
| Repeated retry for the same source/recipe | Jobs reuse validated results or replace them atomically. No duplicate final file races or growing orphan set remains. |
| Disk full or unwritable cache/project directory | The failed write leaves existing assets and state intact. The UI names the storage problem. |
| Missing source or stem cache on restore | Musical edits remain. Relink or regenerate is available. An isolated-stem project cannot silently render the original mixture. |
| Corrupt or oversized state | Validation rejects it safely. No unbounded allocation, crash, invalid frame interval, or NaN parameter enters playback. |
| Editor closes or plugin is removed during a job | Teardown does not dereference the editor or processor after destruction. Job cancellation/continuation follows a documented ownership policy. |
| Host changes sample rate or maximum block size | Repreparation rebuilds the required buffers and stretch state safely. Old-size scratch storage is never reused incorrectly. |

Capture the user-visible error and the technical diagnostic with the same job or session ID. Verify a subsequent successful import or separation and a correct render. Recoverability requires making music again after the failure, not merely surviving it.

## R15. Compare realtime, anticipative, and offline rendering

Requirements are P08, P14, P17, and P21.

1. Use a project with several pad settings, tempo changes, stem automation, and output routing.
2. Capture realtime playback and perform an ordinary offline render with the same range and settings.
3. Repeat the offline render immediately after a cold project open, with the editor closed.
4. Exercise REAPER anticipative processing both enabled and disabled where applicable. Record the actual settings.
5. Render at another sample rate and with the longest supported block size. Verify the source-to-host sample-rate path.
6. Compare onset locations, total duration, stem selection, parameter transitions, and channel routing. Compare samples directly when the engine promises deterministic equality.

Pass when every mode renders the same musical result with the documented tolerance. A missing first bar, stale tempo, wrong pitch, unready stems, or worker-starvation silence fails. Host render speed may exceed realtime by a large factor. The plugin cannot assume a wall-clock interval between callbacks. VST3 explicitly distinguishes realtime, prefetch, and offline processing modes. [VST3 processing modes](https://steinbergmedia.github.io/vst3_doc/vstinterfaces/namespaceSteinberg_1_1Vst.html).

If offline preparation is required, prove its correct lifecycle before the first audible event. An arbitrary delay in the test script conceals the defect. If a failure cannot be reported through the host render API, the product needs a clear readiness mechanism that prevents a successful-looking wrong render.

## R16. Verify the distributable and the scope of the completion claim

Requirements are P01 and P23.

1. Build from a clean checkout with pinned dependencies using the repository's Linux build instructions.
2. Inspect the bundle and resolve all runtime dependencies on the target system. Verify the installed copy's hash against the tested artifact.
3. Run the plugin validator, meaningful processor boundary tests, and the applicable real-host cases above.
4. Inspect the package for source audio, private stems, transient caches, credentials, and machine-specific paths. Exclude them from publication.
5. Verify license notices and source availability for APC, JUCE, DSP libraries, fonts, icons, worker code, and any distributed checkpoints.
6. Review the feature matrix against the manifest. Every essential and adaptable expected feature needs evidence or an explicitly unresolved row.
7. Deliver the actual installed plugin path, tested build identity, concise feature status, and evidence location. State exactly which configurations were exercised.

A local private-use implementation and an unrestricted redistributable checkpoint package have different evidence needs. Do not infer model redistribution rights from the code repository's license. Do not describe an unresolved quality or licensing question as a completed distribution check.

## Require independent review of the evidence

The final reviewer must open the actual reports and selected audio, inspect the current code and state schema, and independently repeat at least the highest-risk cases. Recheck R03 MIDI offsets, R06 live tempo, R08 real stems, R10 cold restore, R11 missing media, and R15 immediate offline render against the final binary. A changed DSP or persistence implementation invalidates the affected earlier evidence.

The reviewer should try concrete failures that the happy path cannot reveal. Start a note one frame before a block boundary. Change tempo before a bank could finish. Hold a long voice while replacing its asset. Close the editor during a job callback. Reopen a collected project with every original path unavailable. Ask for a render before any GUI timer runs.

Prove It Works makes the actual REAPER artifact the authority. Test Behavior, Not Implementation makes independent rendered audio and saved-project recovery the assertions. Model the Domain makes stale job generations, missing assets, and unknown analysis explicit test states. Experience First makes successful recovery, playable MIDI, legible native controls, and honest stem feedback part of completion.
