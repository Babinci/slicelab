# SliceLab implementation run

## Workflow checklist

- [x] Read the Principles section of the poteto-mode skill.
- [x] Phase A: Frame.
- [x] Phase B: Design the workflow.
- [ ] Phase C: Run the loop.
- [ ] Phase D: Keep the audit trail.
- [ ] Phase E: Verify and hand back.

## Definition of done

SliceLab is installed as a native Linux VST3 instrument. The real plugin loads in REAPER and produces finite, non-silent audio from MIDI and its pads. The supplied local sample supports waveform navigation, automatic and manual slicing, movable slice boundaries, independent tempo and pitch, detected BPM and key, host tempo sync, four selectable remote GPU stems, and a resizable editor. Project save and reload preserve sample, slices, stem selection, and controls. Verification records each requirement as VERIFIED, NOT VERIFIED, or INCONCLUSIVE with evidence.

No source-separation model guarantees perfect reconstruction of isolated sources. Model and quality claims require primary evidence. The reference product is a workflow reference. SliceLab uses original source and graphics.

## Designed phases

1. Inventory APC, existing remote AI transport, REAPER, and the build machine. Run three independent context/design workers.
2. Compare whole architecture candidates and freeze the PRD, domain model, ownership boundaries, and verification matrix.
3. Establish a reproducible user-owned Linux toolchain and APC-based build. Verify a minimal VST3 before adding sampler features.
4. Implement the sampler engine, independent tempo/pitch rendering, sample analysis, and persistence with behavior checks.
5. Implement remote separation, cache verification, and failure handling. Run the supplied sample on the GPU.
6. Implement the original resizable editor, waveform manipulation, pads, and all production controls.
7. Run artifact tests, independent review, and the complete REAPER acceptance matrix. Fix defects and repeat affected checks.
8. Package the installed plugin, source repository, usage guide, and evidence. Publish only source and synthetic fixtures.

## Throughput checkpoint

Use at most three workers at once. Research workers have separate output files. Implementation workers own distinct modules against a frozen interface. Build with at most two compiler jobs on this laptop. The first high-risk gates are the missing Linux development toolchain, APC's macOS-specific shell builder, independent time stretching, and real remote GPU execution. A passing build alone never completes a feature.

## Local media

The user-authorized sample is copied to repository-root `sample_test.wav`. The original remains untouched. The sample, generated stems, REAPER projects containing local paths, and audio renders are excluded from the public source repository.

## Verified candidate checkpoint

The installed candidate binary has SHA-256 `1136a85c4251f2c7a22e880a04664a86c7e9d6e9c53aee501452fb6f69a4c928`.
The complete CMake test run passed: 114 engine checks, 36 processor feature checks,
27 scheduled-engine checks (including 288 reverse-duration combinations), 30 marker
and metronome checks (including legacy state migration), and zero allocation failures in the actual processor callback.
The separation integration helper passed 52 checks. The Python helpers passed 24 tests.
Pluginval strictness 5 passed with GUI and bus-layout tests enabled. The coherent REAPER
batch passed 42 host jobs and 42 audio checks; a separate highest-output test passed
with signal only on channels 63–64. Actual GUI and live soak verification are ongoing.

The address/undefined-behavior sanitizer publication stress completed 892 callbacks
while replacing 500 assets, with no invalid samples, unreclaimed assets, or sanitizer
diagnostic. This does not establish every possible concurrency schedule. The initial
ThreadSanitizer run could not start because of an unexpected memory mapping; no race-free
claim is made from that attempt.

The physical machine does not sustain 32 simultaneous R3-stretched voices in the
measured live workload. All 32 work offline and neutral playback is much cheaper.
Keep this throughput limit visible in the testing handoff. Separation is a real model
estimate; no perfect-isolation or universal model-ranking claim is established.


### Concurrency follow-up

Disabling ASLR for the test process allowed TSan to start, and its initial mixed-instrumentation run reported two command-slot races. The FIFO atomic handoffs were in an uninstrumented JUCE core object. The corrected diagnostic target compiles the full JUCE core translation unit with TSan; a linker map verifies that the uninstrumented archive copy is not selected. An intentional race is detected by the positive control. Three unsuppressed stress runs then passed, totaling 8,926 callbacks during repeated source publication, with no diagnostics, invalid samples, or unreclaimed assets. Engine, stress code, and JUCE core are instrumented; other linked JUCE modules and DSP dependencies are outside this test's instrumentation scope.

Actual REAPER gestures found a dropdown race between periodic model refresh and JUCE's asynchronous ComboBox callback. The editor now updates model-backed choices only when their model value changes. Grid interval, source key, and target key share the fix. This changes the candidate binary and requires a fresh host/editor check; earlier hashes remain historical evidence.


### Updated candidate and clean build

The dropdown-fixed installed binary is `7265e6844ee429ceae41aac5315505584c62d85aab069cc4e0ce398a2d905942`, with production source committed in `0fe0151`. Its coherent REAPER batch passed 43/43 host jobs and 43/43 audio checks, now including the highest 64-channel route. GUI-enabled pluginval strictness 5 also passes.

A complete quiet rerun passed 114 engine checks (including two fixture-save checks), 37 processor feature checks including the installed S-KEY analyzer and memory cases, 27 scheduled-engine checks, 30 marker/metronome checks, both allocation gates with zero failures, and 26 Python helper tests. The separate separation integration suite passed 52 checks. A preceding concurrent-build run failed the 96 kHz single-voice audible/overload condition; it recorded no heap operation. Its logs are preserved as `processor-allocations-under-build-load.log` and `gui-fixed-test-run-under-build-load.log`. The quiet pass closes that particular normal-load gate without promising uninterrupted operation under every system load.

A separate clean checkout of `0fe0151` built the VST3 from scratch with clean pinned APC/JUCE/RubberBand sources. It reused the installed local toolchain and extracted development headers through ignored links. Its binary is `03b48ee1eeb9a22086bc607acb02f28d5948f50e959f53518dad02153a8cf691`. The build is source-reconstructible rather than bit-reproducible: development worker fallback paths and JUCE compilation-time strings differ between build locations/times. Runtime dependencies resolve on the host. The installed and host-verified candidate remains unchanged.

The source-package bootstrap was corrected to support bundled vendors without Git metadata. A Gitless bootstrap check passed using the same local tools/headers. Final extracted-archive verification and package checks remain pending. Actual dropdown choices, undo/redo, preset restore, minimum/large resize and fresh-process size/choice persistence passed in REAPER. Final live capture and the long soak are still separate gates.
