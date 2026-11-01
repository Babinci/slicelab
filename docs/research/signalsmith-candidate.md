# Signalsmith Stretch candidate

Do not use this experimental adapter in the audio callback. Its note startup and ratio-change path exceeded a 128-frame callback budget on the test machine. The candidate is not wired into the build. Its quality has not been approved by listening tests.

The tested versions are Signalsmith Stretch 1.3.2 at commit `88061fd4eda56a2353c5d65c74aa3f4ccf76f9da` and its specified Linear 0.6.6 dependency at commit `7bb1c8f36794d6980080ce817c91f8da8fe56a66`. The [upstream README](https://github.com/Signalsmith-Audio/signalsmith-stretch/blob/88061fd4eda56a2353c5d65c74aa3f4ccf76f9da/README.md) says time stretching sounds best between 0.75x and 1.5x. The probes also exercise the requested 0.25x to 4x range.

The experimental `plugins/SliceLab/Source/StretchRenderer.{h,cpp}` uses a 120 ms analysis window, a window/16 hop, split computation, fixed input storage prepared off the callback, and `outputSeek` to align the first output sample. `start` borrows a stereo source buffer, or mirrors its left channel when the right channel is null. `render` writes directly to caller buffers and returns the number of frames produced. It does not clear the rest of the output. Ratio changes re-prime the stretcher from the current logical position, so they can cause discontinuities and have the startup cost again. Pitch changes still have spectral processing latency. This prototype does not implement the engine's source-rate conversion or stem mixing.

## Observations

- The wrapper passed 72 static duration cases, including fractional ratios, and a mid-note ratio change with an expected 84,000-frame duration. At 48 kHz, all 25 combinations of five duration ratios and five pitch factors produced the expected frame count and a 440 Hz sine at the requested pitch within 3%.
- Glibc allocation hooks and explicit C++ `new` and `delete` hooks were active during starts, rendering, and repeated live settings changes at 44.1, 48, and 96 kHz. Their positive-control calls were detected, and the tested operations made zero observed allocation or free calls. This does not prove every signal and configuration is allocation-free.
- On the unchanged library, an isolated 0.25x impulse nearly vanished. At 48 kHz and 128-frame blocks its peak was 0.000053 relative to a unit input impulse. A diagnostic upstream patch changed `silenceCounter += inputSamples` to `silenceCounter += std::min(inputSamples, outputSamples)` in a disposable local header. With that patch, the same test's peak rose to 0.231 and the first output above 1% of the input amplitude was 0.479 ms after the expected event. This supports the hypothesis that the existing silence shortcut clears delayed output before it has drained. The patch needs independent review and more coverage before adoption.
- With the patch, a unit impulse at source frame zero reached its peak and crossed the 1% input threshold at output frame zero for all five tested ratios and 64, 128, and 512-frame blocks at 48 kHz. Later impulses at 4x produced substantial pre-echo. In one 48 kHz, 128-frame case, the first sample above 1% of input amplitude was 59.35 ms early. The peak was 8.52 ms early. Other seeds and call histories can move the phase-vocoder peak.
- A same-source comparison at 4x placed 43.95% of the patched Signalsmith output energy in the interval 50 to 5 ms before the expected impulse. The existing Rubber Band R2 configuration placed 3.92% there. Its 1% threshold crossing was 14.19 ms early and its peak was 7.29 ms late. This single synthetic case is not a listening-quality ranking.
- At 48 kHz and ratio 0.25, 21 measured `start` calls consumed 8.35 to 15.23 ms of thread CPU time, with a 9.34 ms median. A 128-frame block lasts 2.67 ms. At 96 kHz, the same measurement was 18.56 to 31.78 ms, with a 20.14 ms median, against a 1.33 ms block. These are optimized standalone measurements of one voice on a busy eight-core machine, not a full host benchmark. Live ratio changes that re-prime the stretcher also exceeded the block budget. No profiler was run to attribute the cost within startup.

## Reproduction

The main probe and its baseline and patched results are in `local/verification/signalsmith-renderer-probe.cpp`, `signalsmith-renderer-probe.txt`, and `signalsmith-renderer-patched.txt`. It exits nonzero because the peak-timing gate fails at some extremes. The separate threshold measurements are in `signalsmith-onset-probe.cpp` and `signalsmith-onset-{baseline,patched}.txt`. The startup, timing, and Rubber Band comparison probes are `signalsmith-start-onset-probe.cpp`, `signalsmith-timing.cpp`, and `signalsmith-rubberband-comparison.cpp`, with corresponding output files.

The patched header is `local/verification/signalsmith-patched/include/signalsmith-stretch/signalsmith-stretch.h`. It differs from the upstream clone by the one silence-counter line described above. Build the main probe against it from the repository root with:

```sh
g++ -std=c++17 -O2 -Ilocal/verification/signalsmith-patched/include -Ilocal/signalsmith-linear-candidate/include plugins/SliceLab/Source/StretchRenderer.cpp local/verification/signalsmith-renderer-probe.cpp -o local/verification/signalsmith-renderer-patched
local/verification/signalsmith-renderer-patched
```

Replace the first include directory with `local/signalsmith-candidate/include` to test the unchanged library. The local probes and cloned dependencies are scratch artifacts, not vendored production dependencies.
