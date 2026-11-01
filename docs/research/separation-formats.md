# Separation input formats and timeline verification

Measured on 2026-11-01 with synthetic audio only. The source was 3 seconds,
48 kHz, stereo, with deterministic asymmetric noise landmarks. The local
decoder was SliceLab's JUCE 9 build; both machines used libsndfile 1.2.2 and
FFmpeg 6.1.1-3ubuntu5. Remote jobs ran on the configured CUDA worker, using
the pinned model and runtime. Generated audio, reports, and local test cache
are under the ignored `local/verification/formats/` directory.

## The observed mismatch

`scripts/verify-separation-formats.py` measures lengths and cross-correlation
of the actual JUCE and FFmpeg decoded waveforms. Positive lag means JUCE's
signal starts later than FFmpeg's. The direct remote jobs below transferred
each encoded fixture to the production worker under its usual `input.wav`
name; each was uncached, completed with four checksum-verified WAV files, and
each stem was 48 kHz stereo with 144,000 frames.

| Input | JUCE frames | libsndfile and FFmpeg frames | JUCE lag vs FFmpeg | Aligned correlation | Direct remote result |
| --- | ---: | ---: | ---: | ---: | --- |
| WAV | 144,000 | 144,000 | 0 | 1.000000 | Matches JUCE length |
| AIFF | 144,000 | 144,000 | 0 | 1.000000 | Matches JUCE length |
| FLAC | 144,000 | 144,000 | 0 | 1.000000 | Matches JUCE length |
| Ogg Vorbis | 144,000 | 144,000 | 0 | 1.000000 | Matches JUCE length |
| MP3 | 145,152 | 144,000 | +1,105 | 1.000000 | Rejected by SliceLab as misaligned |

The MP3 correlation is rounded; its unrounded value was
0.9999999999986685 after shifting by 1,105 samples. This is a concrete result
for this encoder and file, not a universal MP3 delay. A length-only padding
fix would still shift the signal. The source MP3 SHA-256 was
`55529ad03817a58e2752d909c6f0dc09b7d34796f9376cf28f39bf6954c45ee1`.

## Corrected path and real-worker result

For sources other than readable mono or stereo WAV files, the processor writes
the immutable loaded JUCE audio as stereo IEEE float32 WAV, verifies format and
exact decoded PCM, and atomically publishes the file for the worker. It verifies
the worker's input hash and timeline, then writes a local manifest that binds the
verified stems to the original source hash and records the transport hash
separately. Readable mono and stereo WAV inputs retain their existing source
path and worker cache identity. The source hash is checked before and after the
remote request.

The same MP3 was passed through the changed processor to the real remote
worker. It returned four stems, each 145,152 frames at 48 kHz stereo, and the
processor accepted them with the original source identity. The serialized
transport WAV SHA-256 was
`75f0b4f364d2eb46be2588183b9bd8636ea32e9e13fccd4715d7b477f8e0db50`.
The SHA-256 values of the downloaded 24-bit WAV stems were:

| Stem | SHA-256 |
| --- | --- |
| vocals | `f8a26ee56a7c3d4df1fc2ce5d9e8924eac13ab2f6141e2567c1f296439cbf2df` |
| drums | `e80379b96ac71477e9cdc7daa31ddde7550d7df3aa333dc09a34be1fa3ca0175` |
| bass | `3f001d1c1db54ba402c58a97db85c0e5700c34874765ba5a3bc0b519c9b5fe9a` |
| other | `e31d2d2bade8356afdf914e08088ace0f2d790976fc19ef2e5510ecf1a0f9733` |

The sum of the four stems had a measured zero-frame lag against JUCE's
decoded source (correlation 0.950803 on this fixture). This is a timing check,
not a claim about separation quality. The source/transport/stem hashes above
identify this run, not expected universal output hashes.

The integration helper `tests/verify-separation-integration.cpp` also exercised
a local fake worker. It compared every transported sample with the loaded JUCE
audio, including a mono AIFF fixture with samples above 1.0, and checked
original and transport hash binding, unchanged WAV path and hash, rejecting a
forged transport hash, rejecting a one-frame-short stem,
rejecting a changed source before and during separation, and ignoring a result
superseded by a new source. All checks passed. The optional final argument runs
the real GPU-backed MP3 case; without it the helper uses the fake worker only.
The helper now also includes a four-channel WAV fixture that checks the first
two JUCE channels and their exact transport, plus original source identity;
this added case passed in the main CMake-built helper. The complete fake-worker
integration run passed 52 checks with zero failures; evidence is
`local/verification/separation-integration-candidate.log`.

The processor currently opens source files through JUCE's file-based format
manager, which selects decoders by extension. Therefore a raw MP3 renamed to
`.wav` is rejected during import, before the separation header check runs. The
header check does not by itself add mismatched-extension import support.

## Reproducing the decoder comparison

With the existing SliceLab build and Python environment:

```sh
mkdir -p local/verification/formats
c++ -std=c++20 -O1 -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 \
  -DJUCE_STANDALONE_APPLICATION=1 -DJUCE_USE_MP3AUDIOFORMAT=1 \
  -I vendor/audio-plugin-coder/_tools/JUCE/modules \
  scripts/verify-juce-audio.cpp \
  build/plugins/SliceLab/SliceLab_artefacts/Release/libSliceLab_SharedCode.a \
  -lrt -ldl -lpthread -o local/verification/formats/juce-audio-probe
.toolchain/bin/python scripts/verify-separation-formats.py \
  --juce-probe local/verification/formats/juce-audio-probe \
  --output local/verification/formats
```

This comparison writes generated fixtures and `comparison.json` to the output
directory. Direct remote jobs and the optional real-worker integration create
synthetic entries in the configured remote job cache and consume GPU time;
they are not needed for the local decoder comparison.
