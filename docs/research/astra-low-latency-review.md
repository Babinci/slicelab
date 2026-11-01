# Low-latency R3 decision

Make the pinned, synchronous R3 scheduler meet an exact 12–20 ms delay before adding a prepared-state cache. The existing 4096-frame default is 85.33 ms at 48 kHz and does not meet the instrument requirement. No existing result proves 32 cold simultaneous voices at 20 ms. This review ran no build, rendering benchmark, or heavy diagnostics while the REAPER agent measured host behavior.

The essential outcome is playable pad and keyboard triggering with independent pitch and duration, preserved R3 quality, live stem controls, and dependable REAPER rendering. PDC aligns a recorded arrangement; it cannot hide delayed live monitoring.

## What the current evidence establishes

- `local/verification/r3-startup.py` calls the installed `librubberband.so.2` through Python `ctypes`. Its clock is wall time and its source buffers contain zeros. The 8–20 ms / 15–32 ms observations are complete priming examples, not an indivisible-call bound or measurements of the new pinned FFTW target.
- The patched allocation log establishes zero observed allocations/frees on its sampled reset/setter/process grid, with a positive control. It is not yet an allocation audit of the linked processor under publication, polyphony, and automation.
- R3-short's pitch probe supports retaining R3. The sub-cent peak estimates are constrained by the estimator and do not establish arbitrary-signal accuracy.
- R3's normal short-window start delay is input lookahead. Because a sampler already owns its source, it may compute that lookahead before its output is due. A 1280-sample R3 delay therefore does **not** force 26.67 ms of plugin PDC at 48 kHz. Startup computation is the constraint. Upstream also identifies priming and final calls as potentially expensive. [Rubber Band integration](https://breakfastquay.com/rubberband/integration.html)

New logs arrived during review and were read without rerunning them. `scheduled-engine-poly.log` records zero overloads for 2/4/8 simultaneously started +7-semitone voices at 48 kHz, 128-frame blocks and 960-frame PDC; 16/32 fail before audible output. With 512-frame blocks, 2/4 pass and 8/16/32 fail. Single-voice 12/16/20 ms candidates pass that sampled workload. These are useful partial results, not a 32-voice product pass. The tests currently measure wall time, run 300 callbacks, and do not demonstrate a whole session or repeated onset storm.

Treat 20 ms mono and four-voice behavior as provisional until the actual host suite passes. `scheduled-engine-rates.log` includes 96 kHz/128-frame calls above their 1.333 ms audio period despite zero scheduler overloads. The engine's backlog counter does not measure a missed hardware callback deadline. This remains open alongside short-test polyphony limits.

`benchmarkIndivisible()` always feeds 128 frames, whereas the engine caps each feed by `getSamplesRequired()` as well. Under HighConsistency, available-zero, nonfinal operation, this latter cap reaches at most one analysis window: input reaches the window threshold, a positive input hop is consumed, and input falls below that threshold. The standalone probe can process multiple windows at extreme ratios with input hops below 128. Its largest wall-time value is therefore not automatically the engine's CPU-grain bound. Measure the exact engine operation with both wall and thread CPU clocks. Reducing 128 alone cannot subdivide one stereo analysis/phase/synthesis operation.

## Three immediate changes, without another architecture

1. Keep the reported delay exact. Only queue capacities need powers of two. The previous `nextPow2(wanted)` unnecessarily inflated it; for example 960 intended frames became 1024. This is now source-fixed in `prepareScheduled`. The default still needs to change only after measured gates pass.
2. Bound finalization as well as startup. `R3Stretcher::consume(final=true)` can run several analysis windows and drain its accumulator inside one call, regardless of a 128-frame input cap. The implementer has now replaced scheduled final calls with bounded zero feeding until the logical slice ends; the source was re-read. Tiny and reversed slices need comparison against ordinary rendering, including the preserved tail policy.
3. Replace the provisional service allowance. At 128 frames per feed and `hostFrames / 16` feed credits, total admitted input is at most eight input frames per host frame. Thirty-two sustained stretched voices at duration ratio 1 require approximately 32; at ratio 0.25 they require approximately 128. Thus the old budget fails structurally, even on an arbitrarily fast CPU. Larger PDC merely postpones that failure. The implementer is raising the ceilings and adding a callback-time cutoff; those changes are not performance approval.

For a feed quantum Q and duration ratios r_i, the long-term feed requirement is approximately `hostFrames * sum(1 / r_i) / Q`, before startup and other operations. R3's locally variable output requires reserve beyond that average. A global CPU budget must cover resets, setters, source/stem mixing, filters, output copies, metering, and clock checks too. A cutoff checked before an indivisible call can still overshoot by that entire call.

## Smallest credible next experiment

Use the existing single owner, timestamped requests, captured views, and delayed PCM. Keep the current source algorithm and libsamplerate backend. Benchmark the **linked pinned Release target** with the callback grain actually used by the scheduler, once the host measurement slot is free.

| Candidate plugin delay | 44.1 kHz | 48 kHz | 96 kHz | 192 kHz |
| --- | ---: | ---: | ---: | ---: |
| 12 ms | 530 | 576 | 1152 | 2304 |
| 16 ms | 706 | 768 | 1536 | 3072 |
| 20 ms | 882 | 960 | 1920 | 3840 |

Values are `ceil(rate * milliseconds / 1000)`; they are proposed test settings, not measured passes. Current scheduling requires at least `maxBlock + 1` frames. A host maximum block larger than a candidate therefore excludes that candidate unless the submission algorithm changes. A 16 ms plugin also adds the host/device monitoring delay; report those separately.

Measure separate costs for reset, each ratio setter, each actual R3 feed, retrieve, and mixing. Record the maximum observed indivisible call and the whole callback wall/CPU times. Test 64-, 128-, and 512-frame hardware periods; a one-frame functional test cannot prove a 20.8-microsecond callback deadline. Keep idle credit bounded.

Use these workloads in this order:

1. One cold stretched note, repeatedly triggered, at normal and extreme ratios/pitches; short/reversed/looping slices and source-rate conversion.
2. Thirty-two simultaneous cold notes followed by sufficient sustain to distinguish startup backlog from continuing overload. Use time ratio 1 with non-unity pitch so the direct bypass cannot make this test pass accidentally; then exercise ratio 0.25 and 4. If cold notes fail before output, isolate continuing cost by starting 32 looping voices with `processScheduled(nonRealtime=true)` for about 200 ms, then switching the same engine to realtime for several seconds. This exercises an existing mode transition and requires no cache. Warm-state failure rules out onset caching as a complete solution.
3. Retriggers and stealing during the initial delay, plus note-off, sustain, source replacement, and stem/pitch/tempo changes during startup and sustain.
4. Actual REAPER monitoring and transport, with every output enabled, allocation hooks, zero overloads, exact MIDI-offset/PDC comparison, cold restore, and offline rendering.

For a callback CPU share beta, approximate startup work available before a conservative deadline is `beta * (L - B) / sampleRate`; B is the supported host block size. For illustration, beta 0.5, L 960 and B 128 at 48 kHz give about 8.67 ms of startup CPU across **all** voices, before continuing voices and overhead. This is budget arithmetic, not a benchmark. It explains why a 10 ms single-voice prime cannot simply be multiplied to 32 under a 20 ms target.

Require both cold-burst completion by the first due output and sustained service below the callback allowance. A passing first note plus later overload is failure. A passing offline render establishes correctness, not live capacity. The scheduler remains independent of REAPER anticipative wall-clock cadence because its work runs inside each callback.

## Backend refinements, ranked by smallest change

1. **Measure the pinned FFTW build first.** `build/CMakeCache.txt` currently shows Release `-O2`; do not compare its result with the installed-library startup log as if they were the same binary. Test `-O3` as a controlled variant if needed; do not enable fast-math to obtain a deadline number.
2. **Benchmark FFTW measured plans prepared outside callbacks.** Rubber Band currently selects `FFTW_ESTIMATE`; measured plans can improve transforms on a particular machine. FFTW documents the planning-time tradeoff. If useful, plan once during controlled preparation and initialize buffers afterward. Do not enable Rubber Band's persistent-wisdom path indiscriminately: it adds file I/O and global planning behavior beyond the needed experiment. [FFTW planner flags](https://www.fftw.org/fftw3_doc/Planner-Flags.html)
3. **Only if profiling identifies FFT/math as dominant, compare a float-processing build.** The pinned source officially permits `PROCESS_SAMPLE_TYPE=float`. Pair it with a real float FFT backend; removing `FFTW_DOUBLE_ONLY` by itself will not convert R3, whose default `process_t` is double. Treat this as a numeric-quality change: repeat pitch, transients, long-note drift, automation, and null/error comparisons. No speedup is asserted.

Do not start by replacing libsamplerate with Speex or cutting the R3 analysis window. Upstream's `COMPILING.md` describes libsamplerate as high quality and usually faster than its built-in resampler, and flags possible artifacts for Speex under varying pitch. The current options already select R3's reduced-cost short-window mode. A different backend requires its own allocation grid.

## Prepared onset states: useful acceleration, insufficient guarantee

A ready state must belong to one owner. A background preparation worker can publish a **complete unused stretcher plus its input cursor and retained output prefix** for an exact configuration. The callback takes it once; its later return/reclamation occurs outside the callback. Never let a preparer touch a live voice. Existing view identity/hazard rules must cover the cache as well.

The key includes source/stem identity, host and source rate, quantized boundaries, direction, looping, effective tempo/time ratio, pitch, stem mix, and any beat-map state that changes the onset. Velocity, downstream gain/pan, filter, and envelopes need not be baked into the prefix. Copying prefix PCM alone does not give R3 the matching phase, classifier, resampler, or source-position state for seamless continuation.

This idea fails as the only deadline solution for four concrete reasons:

- Rubber Band has no public state-clone API. A prepared live state is consumed by one trigger; repeatedly striking the same pad needs multiple ready states or a refill-safe cold path.
- A keyboard adds many pitch keys; cents and tempo automation create continuous variants. A finite prepared bank cannot promise a hit for every newly arriving configuration.
- `source()` mixes stems **before** R3. Cached mixed onset PCM cannot reflect a new stem mute/gain at trigger time. Independently caching/rendering original plus four stems changes the algorithm and multiplies storage and continuing DSP; summed independently stretched stems are not guaranteed equivalent to stretching their sum.
- Preparing only the silent left pad avoids some copying/reset work but does not precompute the signal-dependent windows needed for the first audible frame. Its benefit must be measured and does not remove sustained 32-voice cost.

If cold startup fails while sustained playback passes, a small exact-key onset bank can be the next measured prototype. Its acceptance test must include consecutive same-pad hits faster than refill, keyboard chords, immediate edits, and explicit cache misses. A long-latency miss or a silent dropped note does not satisfy the instrument requirement. Do not add a cache until it addresses a measured startup-only bottleneck.

The parent's follow-up proposed a generic zero-prefix pool, independent of asset and note. HighConsistency always resamples after stretching, so the required left pad is indeed independent of pitch/ratio (1280 at 44.1/48 kHz). But that pad is half the 2560-frame window source, so feeding only it performs **no FFT**. It removes reset/zero-copy/call overhead, not signal-dependent priming. Preparing more zeros starts a different signal history and is not an equivalent reset.

Even the short prefix cannot safely be prepared at ratio/pitch 1 and later treated as an untouched reset: `process()` changes mode to Processing; `calculateHop()` then stops initializing previous-hop values; and `consume()` calls `StretchCalculator::calculateSingle()` before its insufficient-input check, advancing that calculator during padding-only calls. A generic-state prototype must compare PCM against exact-settings cold starts, not merely show nonzero output. An internal reset-and-prefill helper could control those states, but is justified only if isolated reset/padding timings show enough savings. It cannot eliminate a slow FFT grain or insufficient warm polyphony. This is the bounded recommendation: run the warm-polyphony discriminator and exact-grain timing before implementing that pool.

## Stem automation has another response time

The scheduler's PDC test measures when note output becomes audible. It does not by itself establish when a new stem mix or pitch setting is heard. The current premix enters R3 at an input cursor ahead of the voice's output cursor, and R3 retains buffered audio. Measure a mid-note stem mute/gain step and pitch/tempo step separately. Upstream notes the lag between changing a ratio at input and hearing its output. [Rubber Band integration](https://breakfastquay.com/rubberband/integration.html)

Moving gains after separate stem stretchers would improve direct gain response but substantially increases DSP and changes sonic equivalence. It is a product/architecture decision, not a free latency fix. Do not promise sample-accurate automation from the current block-level parameter snapshots.

## What stays, what goes, hardest cut

**Stays:** one synchronous R3 owner; exact timestamps and fixed PDC; bounded startup/tail phases; current stem semantics; measured allocator/deadline/REAPER gates. These preserve the musical result and avoid host-specific worker timing assumptions.

**Goes:** power-of-two PDC; the unsustainable feed-credit constant; final-call flushing on the scheduled path; treating old startup logs as new-binary performance; cache-first and worker-first expansion; an 85 ms default presented as playable.

**Hardest cut:** defer an attractive preprimed pad bank. It can improve a warm demo but cannot establish arbitrary keyboard/automation/retrigger behavior or solve insufficient sustained throughput. First obtain the linked one-step, cold-32, and sustained-32 measurements. If those fail at 20 ms after the small backend experiments, state the remaining product gap: the present per-voice R3 architecture has not met the requirement. Shared source analysis or another quality-validated engine would then be a separate redesign, not a reason to silently raise live latency.
