# Corrected beat-map audio measurement

The final installed VST3 rendered all 256 beats of a deliberately drifting 64-bar impulse fixture in REAPER. Peak errors ranged from -1.500 to +2.3125 ms against an independently calculated 120 BPM grid. Eight peaks exceeded 2 ms raw; none exceeded the predeclared 4 ms combined bound. That bound includes the 2 ms design target and a separately declared 2 ms impulse-shape allowance. This is not a strict raw 2 ms result or a universal transient guarantee.

## Fixture and method

The deterministic stereo 48 kHz WAV contains one single-frame, 0.8-amplitude impulse on each of beats 1 through 256. Beat 0 supplies a one-beat lead-in. Each source beat lasts `19800 + 90 * floor((beat - 1) / 16)` frames, followed by a final 21,150-frame interval. Cumulative integer source positions define the impulses. Eighteen anchors at beat 0, every 16 beats through 256, and beat 257 describe source tempo slowing from approximately 145.455 to 136.170 BPM. The 109.6832917-second source occupies 42,118,384 decoded stereo float bytes. Validation checked every impulse and anchor against that independent formula.

The private REAPER 7.78 process loaded the frozen installed VST3, restored the exact state and all 18 anchors, and rendered stereo 32-bit float WAV at 48 kHz without an audio device. One MIDI note 36 began at frame 960, or 20 ms. Hold was off, so the full-length slice continued after note-off. Expected output peak positions were `960 + 24000 * beat` frames. REAPER compensated the plugin's reported 960-frame delay; the analysis did not subtract it again. The reference was a mathematical host-clock grid, not a separately rendered click track. REAPER's offline callback size was not measured.

The independent analyzer searched each nonoverlapping ±200 ms prediction window for an absolute peak above 0.001 and retained every signed error. A separate global cluster count checked for extra or missing transients. The render contained exactly 256 detected beats and 256 clusters, with no non-finite samples.

## Controls and limits

A processor-boundary render of the same fixture at 48 kHz/512 frames, offline and synchronized to 120 BPM, also detected all 256 beats. Its Stretch errors ranged from -1.500 to +2.854167 ms; 33 exceeded 2 ms raw and none exceeded 4 ms. A Repitch control, which traverses the same map without R3 stretching, ranged from 0 to +0.645833 ms. These processor measurements are distinct from the actual VST3 wrapper render.

The initial fixed-ratio R3 calibration and its 2 ms shape allowance were declared before the primary render. All 16 calibration impulses localized within 1.500 ms raw. Two supplementary controls, explicitly declared after the primary result without changing its criterion, measured maxima of 1.125 and 0.875 ms at the middle and final source tempos. They support the allowance for these impulses at three fixed ratios. They do not isolate every contribution to the changing-ratio error or establish a shape bound for other musical material.

The worker execution record confirms that the thresholds were present before the first calibration. A later protocol edit corrected only two descriptive tempo/duration values before the primary Stretch render. The private provenance note retains the original text hash, exact edit and execution-record positions; the current document's modification time alone does not establish that chronology.

## Artifact identity

The installed binary was mapped by the host and had SHA-256 `16249f7b984354fc28f1e810e6bd54b661ebd00405727ffdd2434dd277bf2221` before and after the run. Production source was frozen at `cf2d80a1571df2d72434ddfa968792e0e8cbc40d`. The host run recorded documentation checkpoint `477ffe4c7389f160c35aea9dd8c221f1aec0f7fd`; it changes only documentation and plugin status metadata from that production revision.

Input SHA-256 is `0e05e63442f70d983a7e128f8e20b89355255b095b2ad247c382afa1f6744352`; rendered WAV SHA-256 is `a148507f131981968bbc7d14e474e01da9a79ff9c57ce11f09883d6cb0470bc3`. The protocols, synthetic input, source scripts, exact state, all per-beat CSVs, calibration results, process metadata and audio remain in the ignored `local/verification/beat-map-final/` directory. The host summary is `reaper_summary.json` and its signed measurements are `reaper_per_beat.csv`.
