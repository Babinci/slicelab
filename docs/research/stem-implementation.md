# SliceLab remote stem worker

The local worker invokes ZFTurbo's four-stem BS-RoFormer on `wojtek-ai-pc`. It keeps the model, job files, and worker in SliceLab-owned directories. It uses the existing REAPER Python environment as a read-only dependency and does not change the REAPER worker or stop other GPU processes.

## Command and result

Run the command outside the audio thread. Each request needs distinct absolute result and status paths.

```sh
python3 scripts/stem_worker.py separate \
  --input /absolute/path/source.wav \
  --result /absolute/path/result.json \
  --status /absolute/path/status.json \
  --host wojtek-ai-pc \
  --quality best
```

`--host` and `--quality` are optional. The only recipe currently implemented is `best`. The source must be a nonempty mono or stereo WAV. The worker snapshots and hashes its input before transfer. The remote worker verifies that hash before it runs the model.

The status file is atomically replaced throughout the request. `state` is `preparing`, `connecting`, `uploading`, `processing`, `downloading`, `complete`, `error`, or `cancelled`. `detail` is a bounded human-readable message. `progress` is `null` while work is in progress and `1.0` on completion. A successful exit is zero. Errors exit with code 1; cancellation exits with code 130.

On success, the atomically written result is JSON with `schema: 1`, `input_sha256`, `model`, `model_sha256`, `config_sha256`, `sample_rate`, `channels`, `frames`, and `stems`. `stems` maps exactly `vocals`, `drums`, `bass`, and `other` to absolute WAV paths. The result also records the pipeline, runtime, inference settings, input and output gains, source path, provenance, and each file's size and checksum. The stem WAVs are 24-bit PCM, use the input sample rate and channel count, and contain exactly `frames` samples per channel.

Before publishing stems for playback, the caller must compare its current source generation with the generation captured when it started the worker. The result's input hash identifies the snapshot. The caller owns result and status paths and must not share them between concurrent requests. The worker never publishes placeholder stems or falls back to another model.

## Pinned recipe

The release is [ZFTurbo's BS-RoFormer MUSDB18HQ v1.0.12](https://github.com/ZFTurbo/Music-Source-Separation-Training/releases/tag/v1.0.12). The checkpoint is `model_bs_roformer_ep_17_sdr_9.6568.ckpt` with SHA-256 `3e9daecd70aaed5b5a0d1f861cc4d77eaa45afb3fc6301b1cf32c1be0f5868fb`. The configuration is `config_bs_roformer_384_8_2_485100.yaml` with SHA-256 `d8afb980318d0c08b9c2e24a7adc00d4f3150320c127a7e4de861800d1321939`.

The [author grants MIT use for this checkpoint and requests credit as ZFTurbo](https://github.com/ZFTurbo/Music-Source-Separation-Training/issues/254). The author also identifies MUSDB18-HQ as the training dataset and does not resolve the dataset terms for commercial use. The [MUSDB18 terms](https://sigsep.github.io/datasets/musdb.html) therefore remain a separate consideration.

The recipe uses 44.1 kHz stereo model input, full-precision inference, batch size 1, overlap 8, and no pitch shift. The separator uses the configuration's segment size of 1,101 STFT frames for inputs of at least 10 seconds and its 256-frame short-input override for shorter inputs. The configuration supplied with the checkpoint specifies overlap 2. The [separator's documentation](https://github.com/nomadkaraoke/python-audio-separator) describes the lower setting as a speed tradeoff that can affect seam quality, so `best` uses overlap 8. The recipe runs three passes and averages the original prediction with predictions from swapped stereo channels and inverted polarity after undoing those transforms. Those augmentation transforms follow the [model author's inference implementation](https://github.com/ZFTurbo/Music-Source-Separation-Training/blob/main/utils/model_utils.py). They are a quality-oriented choice rather than a proven improvement on every input.

If the decoded model input peaks above 1.0, the worker applies one input gain before inference. If any combined stem peaks above 1.0, it applies one shared output gain to all stems before converting to 24-bit PCM. The manifest records both gains. FFmpeg decodes the first audio stream explicitly. After inference, FFmpeg converts each output to the input sample rate and channel count. The worker checks that model outputs have the same number of frames as the model input before resampling, and checks every final frame count afterward. Padding and trimming address resampler rounding at the end; they do not compensate for a missing model-output block.

The versioned runtime requires `audio-separator 0.47.0`, `torch 2.13.0+cu130`, `numpy 2.4.6`, `scipy 1.17.1`, `soundfile 0.14.0` with `libsndfile 1.2.2`, `librosa 0.11.0`, `soxr 1.1.0`, `einops 0.8.2`, `rotary-embedding-torch 0.6.5`, and FFmpeg `6.1.1-3ubuntu5`. The remote process checks the versions, the checkpoint and config hashes, and CUDA availability before inference. The worker errors if they do not match. Change the pinned recipe and pipeline identity together when updating the runtime.

## Remote setup

The current host has the worker at `~/.local/share/slicelab-stems/v1/remote_worker.py` and the two pinned assets in its `models` directory. The inference Python is `~/.local/share/reaper-remote-stems/.venv/bin/python`. The worker imports its packages without modifying that environment. It stores jobs under `~/.cache/slicelab-stems/jobs`.

From the repository, install the worker and download the assets on a host that already has the matching runtime:

```sh
ssh wojtek-ai-pc 'mkdir -p ~/.local/share/slicelab-stems/v1/models ~/.cache/slicelab-stems/jobs'
scp scripts/remote_worker.py wojtek-ai-pc:.local/share/slicelab-stems/v1/remote_worker.py
ssh wojtek-ai-pc 'cd ~/.local/share/slicelab-stems/v1/models && \
  curl -fL -o config_bs_roformer_384_8_2_485100.yaml https://github.com/ZFTurbo/Music-Source-Separation-Training/releases/download/v1.0.12/config_bs_roformer_384_8_2_485100.yaml && \
  curl -fL -o model_bs_roformer_ep_17_sdr_9.6568.ckpt https://github.com/ZFTurbo/Music-Source-Separation-Training/releases/download/v1.0.12/model_bs_roformer_ep_17_sdr_9.6568.ckpt && \
  sha256sum config_bs_roformer_384_8_2_485100.yaml model_bs_roformer_ep_17_sdr_9.6568.ckpt'
```

The local `SLICELAB_STEM_CACHE` environment variable changes the local cache. `SLICELAB_REMOTE_WORKER` selects an absolute remote worker path. `SLICELAB_REMOTE_ROOT`, `SLICELAB_REMOTE_MODELS`, and `SLICELAB_SEPARATION_PYTHON` select the remote job root, asset directory, and inference Python. These variables are passed to the remote command. The returned remote job path must contain only ASCII letters, digits, slash, underscore, hyphen, and period.

The client uses batch-mode SSH and the configured `wojtek-ai-pc` alias, including its existing ControlMaster. If authentication expires, run `ssh wojtek-ai-pc` in a terminal, complete the existing login flow, and retry the request. The worker does not open a login window or store credentials.

## Cache, retry, and cancellation

The local cache is `~/.cache/slicelab/stems`. Its key incorporates the source SHA-256, pipeline version, config hash, and checkpoint hash. A per-key lock serializes identical local requests. On every cache hit, the worker checks the manifest, the pinned runtime and settings, all four names, checksums, sizes, WAV formats, channel counts, sample rates, and frame counts. A damaged local cache triggers a fresh download or separation. Remote completed outputs are also verified before reuse.

Remote jobs are detached from the SSH connection and keyed by source and recipe, so a later request can attach after a temporary disconnect. SIGTERM or SIGINT to a local request asks the remote host to cancel the job that request started. The cancelled status records the remote response in `remote_state`. SIGKILL cannot be caught. If the SSH connection is unavailable at cancellation time, the remote job may continue. An attaching request does not cancel a job started by another request. Local and remote caches are reconstructible but are not automatically pruned.

## Verification on 2026-11-01

A real run processed `sample_test.wav` on the RTX 3060 Ti while the existing transcription process remained running. All four WAV files decoded successfully. Each had 1,102,424 frames, two channels, and a 44,100 Hz sample rate. Their finite, nonzero audio passed the file checks. The sum of the four stems had its strongest correlation with the input at zero offset in a search over offsets from minus 12 to plus 12 samples. This is an alignment check, not a source-isolation quality score.

The same input was processed with overlap 2, overlap 8, and overlap 8 with three-pass augmentation. All settings produced different waveforms. The overlap 8 and augmented runs fit alongside the existing transcription process. These comparisons do not establish which setting isolates the sources more accurately because ground-truth stems were not provided.

A separate generated mono 48 kHz input was cancelled during remote work and then retried. The retry completed with its original channel count and all 864,000 frames. The Python protocol tests exercise a verified cache hit, corrupt local and remote files, incorrect frame counts, missing stems, mismatched model identity, authentication failure, stale-result removal, and cancellation. Run them with:

```sh
python3 -m unittest discover -s tests -p test_stem_worker.py -v
```

The source audio has no provided ground-truth stems. The runs do not establish SDR or prove that a stem is free of bleed or artifacts. Listening and ground-truth evaluation remain separate checks.
