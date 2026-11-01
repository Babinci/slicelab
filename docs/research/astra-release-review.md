# Independent release and audit-trail review

Updated 2026-11-01 at approximately 13:44 UTC after the focused follow-up. This supersedes the earlier pending-test statuses in this review. The inspected candidate SHA-256 is `1136a85c4251f2c7a22e880a04664a86c7e9d6e9c53aee501452fb6f69a4c928`.

The new marker, preview-metronome, and canonical separation-input paths have no additional concrete source defect identified by this review. The candidate now has substantial passing host, processor, and allocation evidence. Full production completion remains unsupported by the measured live 32-voice R3 throughput limit and the outstanding final GUI/live/soak/distribution gates.

The reviewer was assigned the independent Astra lane, separate from implementation authors. Actual model-family routing must come from the parent's dispatch metadata, not repository documents. The review used the provided task record, current source, local artifacts, and read-only commands. A full conversation transcript and private reasoning were unavailable; neither was claimed as reviewed. No unrelated chats were searched.

## Current findings and unresolved flags

| Priority | Finding | Evidence and disposition |
| --- | --- | --- |
| P1 / product limit | Simultaneous live stretched polyphony misses the requested 32-voice target on the measured machine. | `scheduled-engine-poly.log` records fail-silent recovery for 16/32 voices at 48 kHz/128 and 8/16/32 at 512. `scheduled-engine-warm.log` shows warmed 32 voices overloading on the first realtime callback. Offline 32-voice success does not resolve continuing live throughput. Keep this visible in the handoff and product status. |
| Validation / unresolved | The TSan retry produced two queue-access warnings; it did not pass. | `local/verification/concurrency-thread-no-aslr.log` ends with 2 warnings after 2,659 callbacks, zero invalid samples, and zero unreclaimed assets. Both warnings identify command-slot reads in `submitScheduled` versus `enqueuePreview`/`enqueueStopPreview` writes. The current CMake target instruments only the stress test and engine, then links ordinary JUCE: its out-of-line `AbstractFifo` atomic handoffs are uninstrumented. Mixed instrumentation is a plausible explanation, not a proved dismissal. Instrument that synchronization and rerun before classifying a real race or claiming a clean TSan result. |
| Validation / open | Final human-facing interaction and long live-session acceptance are separate from API validation. | The assigned host/UI lane was still performing actual gestures, 512-frame monitoring, and the 30-minute soak. Pluginval's GUI-enabled success is useful lifecycle evidence, not proof of file-drop, handle, resize, focus, or listening behavior. |
| Distribution / open | The source revision must reconstruct the tested artifact. | Candidate host identity still names old HEAD `9bf2018`; implementation was uncommitted during inspection. Commit the complete audited source, perform the planned clean-checkout build, and bind package/install hashes to that source. Scan the staged tree and actual package before publication. |

The TSan warning was sent immediately to integration. It supersedes the earlier statement that TSan was only blocked by an unexpected mapping. The initial blocked log remains historical evidence.

## Focused source review

- **Canonical input:** `canonicalSeparationInput` writes immutable JUCE-decoded stereo PCM as float32 WAV in bounded background chunks, rereads and bit-compares every sample, hashes the finished file, and atomically renames it. This preserves MP3 timeline padding and over-unity samples. Non-WAV and multichannel WAV sources take this path; readable mono/stereo WAV preserves its old transport identity.
- **Manifest binding:** `bindSeparationManifest` requires the returned transport hash, rate, stereo channel count, and exact frame count before recording both transport and original source identities. `readStemFiles` then checks every stem checksum, length, rate, and finite sample. Original-source checks run before/after the worker, and generation checks prevent a stale result replacing a later source. No new binding defect was found. The real MP3 timing result remains a timeline test, not a quality score.
- **Markers:** import bounds input at 256 KiB, rejects embedded NUL/invalid UTF-8, strictly parses two CSV/TSV fields, requires finite distinct in-range times, and rejects source-frame collisions after rounding. All validation and capacity checks precede one undoable mutation. Favorites and immutable source ownership survive; generation/source identity are rechecked under the session mutex. Generic text interchange is correctly distinguished from native Serato DJ metadata.
- **Metronome:** the click follows the audible logical source cursor and piecewise beat map, rather than Rubber Band's ahead-of-output input cursor. It uses fixed voice fields, runs only on source preview, is master-gain-scaled, and resets on preview start/stop or disabling. It does not add callback heap ownership or pad/export clicks. Direct/scheduled timing and pad independence have dedicated tests.
- **Legacy parameter migration:** the new `metronome` parameter is appended after existing parameters. Restore adds its default-off child only when exactly one parameter is missing and that child is metronome; the subsequent count, known-ID, uniqueness, and range validation still runs. This does not silently accept a state missing an arbitrary existing parameter. The current fixtures contain the new parameter; a dedicated old-410-parameter restore fixture was not identified in the inspected tests, so legacy behavior is source-reviewed rather than independently runtime-proven here.
- **Prior state/lifetime paths:** local promise ownership is released before restore waits; erased/throwing jobs can therefore ready the future on destruction. Hazard scans follow call → request → acquiring/retained → voice handoffs. ASan/UBSan publication stress supports the lifetime implementation, while the current TSan result needs the instrumentation correction above.

## Verified evidence and closed findings

I opened the artifacts rather than treating the task summary as their contents. No heavy build, GUI operation, benchmark, GPU inference, or new long test was launched by this reviewer.

| Artifact | Observed result |
| --- | --- |
| `local/verification/reaper-runs/20261101-143403-282d77f7/{identity,execution,audio-checks}.json` and `binary-sha256-after.txt` | Same candidate hash before/after; 42/42 host jobs and 42/42 audio checks passed. Includes strict pitch, tempo, MIDI offsets, automation, 32 offline voices, and cold reopen cases. The earlier four R2 pitch failures are closed for this candidate. |
| `local/verification/reaper-runs/20261101-143635-9b168327/audio-checks.json` | Highest output passed at the same candidate hash: 64 channels; channels 1–62 RMS exactly zero, channels 63–64 RMS 0.247894. This is boundary-routing evidence, not a claim that every bus combination was independently tested. |
| `local/verification/pluginval-candidate.log` | Strictness 5, GUI and bus tests enabled, ends `SUCCESS`. |
| `local/verification/engine-tests.log` | 114 checks, zero failures. Includes required checksum rejection, changed-stem collection, state restore, and first-note/gain assertions through declared PDC. The earlier signed-fixture contamination is corrected and passes. |
| `local/verification/processor-feature-tests.log` | 36 checks, zero failures, including memory-budget cases. |
| `local/verification/scheduled-engine-tests.log` | 27 checks, zero failures; reverse fractional-duration tests cover the added 288 combinations. The earlier reverse termination defect is closed for this tested source. |
| `local/verification/marker-metronome-tests.log` | 27 checks, zero failures. |
| `local/verification/separation-integration-candidate.log` | 52 checks, zero failures. Older format logs have smaller counts; this is the artifact supporting 52. |
| `local/verification/processor-allocations.log` | Zero whole-processor realtime callback allocation failures. Source includes allocator positive controls and an audible, non-overloaded single-voice branch; the earlier fail-silent-only coverage concern is closed. This is sampled coverage, not proof of every callback condition. |
| `local/verification/python-tests.log` | 24 tests passed. The initial review independently reran seven stem-protocol tests successfully; the 24-test result is an inspected integration artifact. |
| `local/verification/concurrency-address.log` | 892 callbacks during 500 source replacements; zero invalid samples, zero unreclaimed assets, no ASan/UBSan diagnostic in this run. The stress source uses neutral voices and offline callback mode; it is not an R3 performance test or proof of all concurrency schedules. |

The anti-phase magnified-waveform fix was re-read: it now uses both channels' extrema instead of averaging cancelling channels. Its code defect is closed; actual zoom interaction remains with the GUI lane. Required checksum rejection and promise-lifetime corrections also remain present.

## Attention audit of the decision trail

Rows refer to `docs/decisions.tsv` including its header. Historical decisions should remain, with new outcome rows appended using captured timestamps.

| Row / timestamp | Current interpretation or remaining flag |
| --- | --- |
| 2–4 / initial local-time placeholders, corrected `11:59:24Z` | Row 4 explicitly disavows the initial event timestamps. Do not reconstruct elapsed work from those placeholders. |
| 5–6 / `11:59:24Z` | Architecture/toolchain choices are supported, but neither choice proves live 32-voice throughput or a clean-checkout build. Apt development packages are not fully version-pinned; record the actual toolchain used for reproducibility. |
| 7 / `11:59:24Z` | Real GPU execution and variants are established. No ground-truth isolation score or universal model winner is established; the internal `best` recipe label must not become that claim. |
| 8 and 12 / `12:45:58Z`, `13:41:20Z` | Historical R2 pitch blockers are closed by candidate-specific 42/42 evidence. Preserve the failed baseline; do not keep calling the current candidate's strict pitch gate pending. |
| 9 and 15 / `12:45:58Z`, `13:41:20Z` | Synchronous scheduling removes the worker wall-time assumption; exact PDC and sampled correctness now have evidence. A soft time cutoff still cannot guarantee deadlines, and row 15 correctly retains the live 32-voice deficit. |
| 10–11 / `12:45:58Z` | Library-only allocation and pre-integration feature checkpoints have now been supplemented by final processor/unit evidence above. Their historical wording need not be overwritten. |
| 13 / `13:41:20Z` | Canonical PCM is justified by a measured decoder mismatch. Record 1,105 as the measured lag, not the frame-count difference: lengths were 145,152 versus 144,000, a difference of 1,152. The current row's wording “differed by 1105 frames” is ambiguous and should say timeline lag. |
| 14 / `13:41:20Z` | Metronome and generic markers are implemented and tested. This closes those adaptable items, not native Serato cue metadata, marker colors, SLAB hardware, or untested host MIDI learn. |
| 16 / `13:41:20Z` | Pluginval success is tied to the candidate hash. Keep actual GUI gestures, live capture, and soak as separate evidence. |

`WORKLOG.md` now checks Frame and Design; implementation/audit/handback remain open. Update these and APC status only from final outcomes. Do not set full shipping readiness while a required live-polyphony target remains unmet. The public REAPER narrative should incorporate the candidate results while retaining its explicitly historical baseline.

## Remaining release boundaries

Ground-truth separation quality, the specified 64-bar drifting beat-map audio measurement, universal 192 kHz/variable-block operation, native proprietary interoperability, and every possible failure/concurrency schedule are not established by this focused review. Do not silently expand the passing matrix to those claims. Actual GUI, live/soak, corrected TSan, clean-checkout build, staged privacy, and package identity have active owners or are explicit remaining gates.

The initial publication-candidate scan found no audio, model weights, compiled binaries, archives, or key files among its 63 regular files. That scan is historical and does not cover later files or an eventual package. The private recording and generated evidence remain in ignored locations. Earlier dependency inspection found host-resolved libraries and no RPATH/RUNPATH in the then-current VST3; verify the final package identity separately. Dependency/source notices were read; this review does not independently certify external checkpoint/dataset permissions.
