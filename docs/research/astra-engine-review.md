# Astra engine review

This is a bounded review of the in-progress engine, processor state, and separation integration. It is not final approval of a linked VST3. Production files changed during the review. Findings marked source-fixed were re-read after the integration lead applied fixes. Other findings describe the recorded draft and need independent verification against the final artifact.

The last recorded source read was 2026-11-01 12:08:25 UTC. Source paths below are relative to the repository root. The review excluded the editor and did not run Ninja or model inference.

| File | SHA-256 at the recorded read |
| --- | --- |
| `plugins/SliceLab/Source/Domain.h` | `4fe4a3c95cd545d72165c95a85dac513eca4a0257419e66a786699ab9f8624eb` |
| `plugins/SliceLab/Source/SamplerEngine.h` | `2aa8335741dd6b3cdb4970ddd348691256d5ced3a139800bc0e682c4078b978a` |
| `plugins/SliceLab/Source/SamplerEngine.cpp` | `718a45622a6b531194c6a71ba4763a839a5f8ac6406e60ff8f14352329a2909a` |
| `plugins/SliceLab/Source/PluginProcessor.h` | `a5f7921612210a616f8306aca4df8db48cd378e5d65592a4ed20327509467380` |
| `plugins/SliceLab/Source/PluginProcessor.cpp` | `1be5dab454fc827dc96704b20c007e9522b6646929a137906f6ec128d7b6e475` |
| `scripts/stem_worker.py` | `29f2cd4baf012fd33e355e9a91361f52ba174c3df012c7eb603a59bcef725f92` |
| `scripts/remote_worker.py` | `559236ed728cefc05da87f397e7e991f295a9ee7637ab62f5004fff63981e56a` |
| `tests/engine_tests.cpp` | `14be28e55974493168f3af76b898e32a4d8ed85c7b67d9872f1c77d12f57ef1a` |
| `tests/test_stem_worker.py` | `8ad76cdbd543ee17c28d9496f871ea229bd9cd219cc3765b52a039dee90eec76` |

## 1. Critical, P1. Allowed pitch settings allocate on the audio thread

**Status.** Open at the recorded read. Measured against the actual linked Rubber Band library.

**Location.** `plugins/SliceLab/Source/SamplerEngine.cpp:201`, `configureStretch`; `SamplerEngine.cpp:273`, dynamic stretch configuration; `SamplerEngine.cpp:354`, `prepare`.

`prepare` constructs each stretcher at its default ratios and sets its maximum input size. The first stretched note calls `reset`, `setTimeRatio`, and `setPitchScale` from `renderVoice`, inside `processBlock`. Preconstructing the object does not reserve every supported pitch regime.

The preserved malloc-interposition probe uses the same library, rate, channels, flags, and maximum process size. A fresh `reset(); setTimeRatio(1); setPitchScale(.25)` performs **4 allocations and 33 frees**. Pitch .25 is the normal supported -24-semitone setting. The other sampled fresh pitches, .5, .8, 1, 2, and 4, did not allocate in that operation.

A preliminary warmup did not establish a remedy. After processing a complete pitch sweep, the next sweep still frees memory at .25 and .5 transitions. Constructing at time ratio 4 and pitch .25 also permits later allocations when changing supported ratios. These observations do not claim that every possible preallocation strategy fails.

**Reproducer.** Build and run `local/verification/astra-rubberband-allocations.cpp`. Its output is preserved in the matching `.txt` file. The probe feeds `clamp(getSamplesRequired(), 1, 1024)`. Compile from the repository root with:

```sh
c++ -std=c++20 -O2 -I.deps/usr/include local/verification/astra-rubberband-allocations.cpp /lib/x86_64-linux-gnu/librubberband.so.2 -o local/verification/astra-rubberband-allocations
local/verification/astra-rubberband-allocations
```

**Minimum fix.** Prepare a renderer whose supported note-start and automation transitions have a verified allocation bound. If Rubber Band needs a reserve or allocator change, make that change outside the callback. Retest reset, setters, processing, and destruction separately. A warmup-only change needs the same probe over subsequent transitions before this finding can close.

## 2. Critical, P1. Hazard-pointer promotion could free the next audio view

**Status.** Source-fixed during review. A concurrent reclamation test remains necessary.

**Location.** `SamplerEngine.cpp:73`, `updateView`; `SamplerEngine.cpp:378`, `collectRetired`. The demonstrated draft was engine SHA-256 `2acc127d24668d2a20e0c9bf6b9ca484c7ba2421e4e2f6c7c18e88141614059d`.

The earlier collector read `retained` before `acquiring`. This valid interleaving loses protection even with the added per-voice hazards:

1. The audio thread loads view N, publishes `acquiring=N`, and confirms that N is current. It has no voice referencing N yet.
2. The publisher replaces N with P and retires N.
3. The collector reads the old `retained=O`.
4. The audio thread promotes N to `retained`, assigns `audioView=N`, and clears `acquiring`.
5. The collector reads `acquiring=null`, finds no voice hazard for N, and frees N.
6. The audio thread starts a note or reads the beat map through the freed `audioView`.

**Reproducer.** Add scheduling barriers at those exact loads and stores in a disposable sanitizer build. No probabilistic timing assumption is needed to permit this schedule. The review did not claim to have observed an actual crash.

**Minimum fix and recheck.** The recorded draft now scans `acquiring` before `retained`, which closes this promotion interleaving under the current sequentially consistent operations. Preserve that ordering and test view promotion plus active-voice retirement under ASan. The earlier unconditional voice reset on source replacement was also removed and replaced with per-voice view retention.

## 3. Critical, P1. The startup delay loop could leave stretched notes permanently silent

**Status.** Source-fixed during review. The corrected library-level probe passes all 24 sampled combinations. Full engine onset and duration checks remain separate.

**Location.** `SamplerEngine.cpp:293`, the stretcher delay-draining loop. The defective condition was `r.available() <= v.outputDelay`.

When available output becomes greater than the remaining delay, the defective loop stops without draining the rest of the delay. The subsequent `outputDelay > 0` check exits rendering. The next block sees the same values and makes no progress.

**Reproducer and evidence.** `python3 local/verification/astra-stretch-startup.py` reproduces the original condition against the installed library. At 44100 Hz, ratio 1 and pitch 2 leave 38 delay samples and 66 available samples. Ratio 2 and pitch 1 leave 19 delay samples and 135 available samples. All 24 sampled combinations at 44100, 48000, and 96000 Hz get stuck. Original output is preserved in `astra-stretch-startup-original.txt`.

**Minimum fix and recheck.** Continue while delay remains or no output is available, always draining `min(available, remainingDelay)`. The source now has that condition. Running the probe with `--fixed` reports zero stuck cases, preserved in `astra-stretch-startup-fixed.txt`.

## 4. Critical, P1. Sub-frame looping slices could spin forever

**Status.** Source-fixed during review. Boundary quantization and constant-time wrapping are present in the recorded draft.

**Location.** `SamplerEngine.cpp:189`, `advance`; `PluginProcessor.cpp:34`, pad validation; `PluginProcessor.cpp:573`, state pad parsing.

The earlier validator accepted any finite positive slice length. Loop wrapping repeatedly subtracted that length until the cursor returned inside the interval. For a slice `[0, 1e-30]` seconds at 44100 Hz, subtracting the length in frames from cursor 1 produces exactly 1 in double precision. The callback loops forever.

**Reproducer.** Set a looping pad to those boundaries through `setPad` or a valid state chunk, then send a note-on. The numerical condition is deterministic, `1.0 - 44100 * 1e-30 == 1.0`. The review checked that equality instead of hanging the audio process.

**Minimum fix and recheck.** The draft now quantizes boundaries to source frames, rejects nonfinite boundaries, guarantees at least one frame for an active slice, and wraps with `fmod`. Test this case and a one-frame loop with the maximum supported speed ratio.

## 5. Critical, P2. Collect can publish media that cannot restore

**Status.** Open at the recorded read. Proven by the copy and restore execution paths.

**Location.** `PluginProcessor.cpp:742`, `collectSample`, especially the source copy at 749 and stem copy at 759; `PluginProcessor.cpp:604`, source hash validation on restore.

Collect copies the current disk paths without comparing their contents against the loaded source hash or the recorded stem checksums. The destination collision check verifies only an existing destination. The newly copied source keeps the old in-memory `sourceHash`.

**Reproducer.** Load source A. Replace A's disk contents with a different valid WAV while the in-memory sample remains playable. Collect into an empty directory and save. Collection reports success. Reopening the saved state rejects the collected source because its bytes no longer match the saved hash. Replacing a cached stem on disk causes the analogous collected-stem failure.

**Minimum fix.** Verify the source and each stem against their durable identities before copying. Write to temporary destinations, verify the copies, and publish the new locations only after the full set passes. On mismatch, retain the current asset and paths and report the changed media.

## 6. Critical, P2. Pending stem references still disappear through collect or undo

**Status.** Partly fixed during review. The direct restore-save path was fixed at `getStateInformation`. Collect and undo still lose the reference in the recorded draft.

**Location.** `PluginProcessor.cpp:535`, saved stem reference; `PluginProcessor.cpp:246`, `Entry`; `PluginProcessor.cpp:354`, `restoreEntry`; `PluginProcessor.cpp:742`, `collectSample` and its final `pendingManifest=copy->stemManifestPath` assignment.

When source decoding succeeds but one stem is missing, `readStemFiles` returns before assigning the asset's `stemManifestPath`. The processor retains the expected path only in `pendingManifest`. The revised save path correctly uses it when `stemsReady` is false. However, collect overwrites it with the copied asset's empty path. Undo entries also omit it, so loading a different sample clears the path and undo cannot restore it.

**Reproducer.** Restore a separated preset with one stem temporarily renamed. Either collect its still-valid source, or load a different sample and undo. Save, restore the missing stem, and reopen that saved state. The expected manifest reference is absent, so automatic stem recovery cannot occur.

**Minimum fix.** Make the expected stem identity and manifest location durable session data, captured by snapshots and undo. Readiness belongs to the decoded asset. As a smaller fix, preserve the expected reference in collect and include it in every undo entry.

## 7. Critical, P2. Recorded separation gains are never undone

**Status.** Open at the recorded read. Proven by the worker and decoder paths. The supplied sample has unity processing gains and does not exercise this case.

**Location.** `scripts/remote_worker.py:207`, input scaling; `remote_worker.py:236`, common output scaling; `remote_worker.py:253`, gain metadata; `PluginProcessor.cpp:170`, `readStemFiles`.

The worker scales inputs above unity by `input_gain` and scales the model outputs by `output_gain` before writing PCM24. It records both factors. The C++ decoder never reads `processing_gain` and plays the scaled estimates directly. The exact-original bypass therefore operates at the original level while isolated stems operate at the reduced processing level.

**Reproducer.** Use an accepted float WAV whose peak is 2.0. The worker applies input gain .5. With output gain 1, even a perfect separator's resulting stem audio is 6.02 dB below the source scale. A model result whose peak exceeds unity produces an additional common attenuation. A deterministic stub separator can establish this without another model run.

**Minimum fix.** Validate both gain factors and undo their product after decoding all stems, or export float stems restored to source scale. Keep this common correction separate from user stem gains. Test with non-unity input and output gains and verify the original bypass remains unchanged.

## 8. Critical, P2. A stale note-off releases a replacement mono voice

**Status.** Open at the recorded read. Proven by the MIDI execution path. The existing repeated-note test covers polyphonic mode.

**Location.** `SamplerEngine.cpp:113`, mono voice reset; `SamplerEngine.cpp:139`, `stop`.

Mono retriggering discards the earlier voice and its outstanding note identity. A subsequent note-off searches only live voices and releases the newer voice with the same channel and note. Voice stealing at the 32-voice limit has the same problem.

**Reproducer.** Use Hold, Mono, a looping pad, and zero release time. Send note-on 36 at sample 0, another note-on 36 at 50, and the first note-off 36 at 100. The second note remains logically held, but the engine releases it at 100. The second note-off has nothing left to release.

**Minimum fix.** Track outstanding note-on instances independently of active voice storage. Consume the oldest matching note instance on note-off, including a stolen or reset instance, before deciding which live voice to release. Exercise mono overlap and pool stealing as well as the existing polyphonic overlap test.

## 9. Critical, P2. Cold restore can bypass stem checksum validation

**Status.** Open at the recorded read. Proven by the manifest parser. This path bypasses the Python worker's stricter validation.

**Location.** `PluginProcessor.cpp:189`, optional `files` and per-stem metadata branches in `readStemFiles`; `PluginProcessor.cpp:605`, direct manifest loading during restore.

If a manifest omits `files`, or omits a single stem's metadata object, the C++ decoder skips checksum verification for that stem. It then accepts any finite audio with the expected dimensions and marks all stems ready. The local worker always emits those records, so absence is malformed persisted data rather than a normal output variant.

**Reproducer.** Copy a valid local result manifest, remove its `files.vocals` entry, and replace the referenced vocal WAV with a different same-length, same-rate WAV. Restore state referencing that manifest. The decoder accepts the changed audio because the missing entry bypasses the comparison. `validate_manifest` in the Python worker would reject the incomplete metadata.

**Minimum fix.** Require complete metadata and a valid digest for every stem before decoding or publishing the set. Keep the original and expected stem references on validation failure. Add the malformed-manifest case to cold restore verification.

## Verification limits

The preserved probes establish the library startup state and allocator calls. They do not establish full-plugin onset accuracy, output duration, audio quality, worst-case 32-voice CPU cost, or sample-accurate host automation. No promise deadlock was demonstrated. No security exploit or GPU result defect was inferred from an untested possibility.

Final checks still need the stable linked processor, cold offline restore, collectible media moved away from original paths, independent pitch and tempo transitions, isolated-stem persistence, and concurrent publication under sanitizers. These are test gaps, not additional findings.

Prove It Works changed this review by requiring executable Rubber Band probes and by keeping source-fixed findings separate from final artifact verification. Throughput checkpoint is not applicable to this bounded read-only review.
