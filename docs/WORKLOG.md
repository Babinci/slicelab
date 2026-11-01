# SliceLab implementation run

## Workflow checklist

- [x] Read the Principles section of the poteto-mode skill.
- [ ] Phase A: Frame.
- [ ] Phase B: Design the workflow.
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
