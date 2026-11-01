# Remote stem separation for SliceLab

Research snapshot, 2026-11-01. This is a read-only investigation. No models were installed and no separation jobs were started.

## Recommendation

Give SliceLab one asynchronous **Separate stems** action and four switches for vocals, drums, bass, and other. The original sample remains playable while work runs. Publish a result only after all four files have passed integrity and alignment checks. The plug-in must never perform network access, process control, audio decoding, or model inference on the audio callback.

The existing remote worker proves that the SSH transport and a four-stem recipe are available. Its cached Demucs weights have a material use restriction. The Demucs author states that the pretrained weights are outside the code's MIT license and are provided only for scientific purposes. Do not assume that installation or private access grants permission for creative production. [Demucs weights discussion](https://github.com/facebookresearch/demucs/issues/327).

For a stronger practical candidate with an explicit weight grant, evaluate ZFTurbo's four-stem BS-RoFormer checkpoint `model_bs_roformer_ep_17_sdr_9.6568.ckpt`. Its author says that this specific checkpoint is MIT licensed, asks for credit as ZFTurbo, and notes that the implications of the MUSDB18-HQ training-data terms require separate consideration. This checkpoint is not currently cached on the remote PC. [Model release](https://github.com/ZFTurbo/Music-Source-Separation-Training/releases/tag/v1.0.12). [Author's license clarification](https://github.com/ZFTurbo/Music-Source-Separation-Training/issues/254). [MUSDB18 terms](https://sigsep.github.io/datasets/musdb.html).

SCNet XL IHF is another high-quality four-stem candidate worth evaluating if its specific weight terms are confirmed. Its author's published test averages exceed the published BS-RoFormer averages in the same catalog, but that does not establish a listening preference for the supplied sample or a working memory footprint on this GPU. An October 2026 request for this particular SCNet checkpoint's weight license was still awaiting an explicit reply in the cited issue. [Model catalog](https://github.com/ZFTurbo/Music-Source-Separation-Training/blob/main/docs/pretrained_models.md). [Checkpoint license discussion](https://github.com/ZFTurbo/Music-Source-Separation-Training/issues/245).

No source-separation model can guarantee a perfectly isolated source. Musical parts overlap in time and frequency. Listen for bleed, missing transients, phase changes, and artifacts rather than interpreting a leaderboard score as a guarantee. [MSST research paper](https://arxiv.org/abs/2607.23395).

## Observed machine and files

- A noninteractive SSH request through the `wojtek-ai-pc` alias succeeded on 2026-11-01. The existing SSH ControlMaster was running. Effective settings include `ControlMaster auto`, `ControlPersist 600`, and a Cloudflare SSH proxy.
- The GPU was an NVIDIA RTX 3060 Ti with 8,192 MiB total VRAM. `nvidia-smi` reported 2,717 MiB already used. A running `live-transcript` Python process occupied 1,956 MiB. This is a snapshot, not reserved capacity. Do not stop or disrupt that process.
- Remote worker: `/home/wojtek/.local/share/reaper-remote-stems/remote_worker.py`. Remote model cache: `/home/wojtek/.cache/reaper-remote-stems/models`. Remote job root: `/home/wojtek/.cache/reaper-remote-stems/jobs`.
- The remote worker's SHA-256 matches the workspace copy. The installed local client also matches the workspace copy. Their SHA-256 values are `ab16560b394838976958df7c35719f41e4ae972d099332a5f666db59ce939f96` and `2093a982d061fbd8059a64f967afa552a218f5ee9fa2bb49da6356697668415a`, respectively.
- The remote Python 3.11 environment reports `audio-separator 0.47.0`, `torch 2.13.0`, `onnxruntime-gpu 1.29.0`, and `soundfile 0.14.0`. The package contains BS-RoFormer support, but compatibility with the proposed checkpoint has not been tested. The inspected architecture directory does not establish SCNet support.
- The named input [`sample_test.wav`](/home/wojtek/Documents/ChatGPT/slicelab/sample_test.wav) contains stereo 16-bit PCM at 44,100 Hz, with 1,102,424 audio frames, or 24.998277 seconds. It also has a PNG stream. Export or read the audio stream explicitly.
- The separate [key-detection tool](</home/wojtek/Documents/ChatGPT/music making/tools/key-detection/README.md>) is a local CPU workflow using S-KEY and Essentia. It does not provide four-stem separation and does not need to be in the stem job.

The observed remote model cache contains these files and their model dependencies:

| Model | Existing recipe | Output | Practical qualification |
| --- | --- | --- | --- |
| `htdemucs_ft.yaml` and four Demucs `.th` files | `music4` | vocals, drums, bass, other, and a `no_drums` residual | Already cached. The author's scientific-purpose weight statement above limits assumed use. |
| `htdemucs_6s.yaml` and its `.th` file | `music6` | vocals, drums, bass, guitar, piano, and other | Already cached. Demucs describes the six-source model as experimental and notes piano bleed and artifacts. [Demucs documentation](https://github.com/facebookresearch/demucs). |
| `vocals_mel_band_roformer.ckpt` | `vocals2` | vocals and instrumental | Already cached. This is a two-output recipe, not a four-stem replacement. Verify the exact checkpoint's provenance and terms before use. |
| `MDX23C-DrumSep-aufr33-jarredou.ckpt` | `drumkit6` | six drum-kit components after Demucs drum extraction | Already cached. It is not an end-to-end four-stem music model and depends on Demucs in the current recipe. Verify its checkpoint terms. |

The remote cache did not contain the proposed ZFTurbo BS-RoFormer or SCNet XL IHF files in the inspected model and shallow cache directories. Another copy elsewhere on the machine was not ruled out.

## Candidate tradeoffs

| Candidate | Evidence | Decision |
| --- | --- | --- |
| BS-RoFormer, ZFTurbo four-stem `ep_17` | The author's model catalog reports 9.65 average SDR on MUSDB test and 9.38 on Multisong. The author separately grants MIT for this checkpoint, with a training-data qualification. | First implementation candidate. Pin the exact configuration and weight hash. Measure VRAM use and listen before making it the default. |
| SCNet XL IHF, ZFTurbo `ep_36` | The same catalog reports 10.08 average SDR on MUSDB test and 9.92 on Multisong. The author identifies its training set as MUSDB18. | Compare if the checkpoint's terms are clear and the model fits while the user's other GPU work runs. The present environment and 8 GiB fit are unverified. |
| Existing `htdemucs_ft` | The remote already has all weights and the integrated recipe. Four independent target models produce the four output parts. | Keep as an existing research capability. The upstream author describes the model weights as scientific-purpose only. |
| Cached MelBand RoFormer | The worker returns a vocal estimate and an instrumental complement. | Useful for a future vocal-only option. It does not independently provide bass, drums, and other. |

The catalog's published scores are model-author results on its named datasets. They are not measurements made in this investigation, and they do not prove the same ordering on a 25-second sampler input. The original [BS-RoFormer paper](https://arxiv.org/abs/2309.02612) and [SCNet paper](https://arxiv.org/abs/2401.13276) describe the respective architectures. The MSST authors discuss cross-fades, test-time augmentation, and ensembles in their [2026 paper](https://arxiv.org/abs/2607.23395). Each can increase work, so use them only after testing this PC and the chosen sample.

## Existing transport and its limits

The relevant local source is [reaper_remote_stems.py](</home/wojtek/Documents/ChatGPT/music making/tools/remote-stem-separation/reaper_remote_stems.py>). The remote source is [remote_worker.py](</home/wojtek/Documents/ChatGPT/music making/tools/remote-stem-separation/remote_worker.py>). The [REAPER integration](</home/wojtek/Documents/ChatGPT/music making/tools/remote-stem-separation/README.md>) renders a WAV, launches the client in the background, and polls a status file.

The client hashes the WAV bytes, protocol version, client version, and recipe into a result key. A per-key `flock` serializes identical local requests. On a cache miss, it creates a random remote job ID, calls `prepare`, uploads `input.wav` with `scp`, waits for `run` over SSH, retrieves a JSON manifest, downloads the listed FLAC files, and checks their sizes and SHA-256 hashes. It then publishes a local JSON and TSV manifest and requests remote cleanup. A cache hit verifies each file's size and hash again.

The worker accepts a short fixed recipe list and confines validated lowercase hexadecimal job IDs to its job directory. Its manifest includes input hash and audio metadata, recipe, model names, output names, sizes, hashes, sample rates, channel counts, durations, and elapsed time. Its `music4` recipe uses `htdemucs_ft` with two shifts and produces four stems plus `no_drums`.

The `no_drums` file is a subtraction residual, not a fifth independent stem. The worker can apply global gain to prevent that residual from clipping and records the gain. Do not mix it alongside vocals, drums, bass, and other.

The client's fixed 5%, 15%, 35%, and 82% values describe phases, not measured inference progress. It updates download progress after each file. The remote worker writes a JSON processing status, but the current CLI exposes no status command and the client waits synchronously for `ssh ... run`. An interrupted connection has no supported automatic reattachment. The remote process might continue or stop; verify its actual state before retrying or deleting anything.

The current cache key does not include the remote worker version, checkpoint hash, model configuration, inference settings, or decoder settings. Changing a model under the same recipe can therefore reuse an old cached result. The client validates manifest filenames and rejects duplicates, but a SliceLab integration should additionally require exactly the four expected unique stem names and validate all hash, size, audio format, frame count, and channel fields. The current remote `cleanup` does not acquire the worker lock, so cleanup must not race a running job.

The local client writes status with an atomic replacement as `key=value` lines. States include `connecting`, `uploading`, `processing`, `downloading`, `complete`, and `error`. On completion, `manifest` points to a TSV file with final local paths. A successful transfer can still surface as an error if the later remote cleanup call fails. The next request may find the completed cache entry.

## Authentication and recovery

The effective SSH proxy is `/home/wojtek/.local/bin/codex-cloudflare-ssh-proxy`. Its source explicitly permits automatic use of a cached Cloudflare token but only launches interactive login from a terminal. If the token is missing, a background client fails with instructions to run `ssh wojtek-ai-pc` in a terminal and complete the browser login. The proxy uses Cloudflare's login lock and a five-minute login timeout. Existing multiplexed sessions can continue while valid.

Do not put credentials or bearer tokens in plug-in state, logs, or the UI. Keep `wojtek-ai-pc` as the host alias so existing SSH configuration and ControlMaster behavior apply. Run `ssh`, `scp`, and any authentication probe outside the audio thread. Capture a bounded error message and show a retry action. Authentication can expire after a successful probe, so handle failures during each transfer and run as well.

For the first version, reuse the installed client as a subprocess only if the selected recipe and its terms are appropriate. Use `juce::ChildProcess::start` with an argument array rather than a shell command. Supply a real WAV file, a unique status-file path, and an explicit output root. Drain or redirect child output so a full pipe cannot stall it. `readAllProcessOutput` and waiting for process exit are blocking operations and belong on a worker thread. [JUCE ChildProcess documentation](https://docs.juce.com/develop/classjuce_1_1ChildProcess.html).

A stronger model needs an isolated, versioned remote runtime and recipe or a separate worker that returns an equivalent manifest. Do not overwrite the working REAPER environment or modify its recipe in place. Keep model downloads, version changes, and GPU tests as a separate implementation phase. No such change was made in this investigation.

## Playback integration

Represent a result as the source content identity, source generation, sample rate, frame count, four named immutable audio buffers, model identity, and inference settings. A background controller owns child processes, transfer, manifest validation, decoding, resampling, and failures. Each request captures a source generation ID. If the user loads a different sample before a result arrives, discard that result for playback while retaining any valid cache entry.

Load and validate all four outputs off the audio thread. Require one `vocals`, one `drums`, one `bass`, and one `other` buffer. Establish a common rate, channel layout, frame count, and sample origin before publishing. Do not trust rounded manifest duration as proof of frame alignment. Keep the audio callback bounded to already-loaded memory, sample playback, and gain smoothing. Publish immutable prepared buffers through a realtime-safe handoff, and reclaim old buffers away from the callback. [JUCE audio callback guidance](https://docs.juce.com/develop/classjuce_1_1AudioProcessor.html).

Use the original source when all four switches are on, and silence when all four are off. For intermediate selections, use one documented mixing policy and compare it by ear. Possible policies are summing selected estimates or subtracting muted estimates from the original. The estimates need not reconstruct the original exactly, so these policies can sound different. Smooth changes to avoid clicks, keep all stems on the same playback position, and do not mix `no_drums` as another stem. The UI should show that separation is pending, ready, or failed rather than presenting fixed phase markers as precise progress.

A sample or switch setting saved in a DAW project must restore reliably after the plug-in or host restarts. Treat a cache as reconstructible, verify its contents, and provide a clear missing-source or retry path if the project moves to another machine. Include the decoded source identity, model and checkpoint hashes, recipe parameters, and relevant format conversion settings in the new cache key. Do not silently fall back to an old model's cached files after a model update.

## Verification still required

1. Resolve the chosen checkpoint's use terms for the intended application. Record its source, exact version, and SHA-256. Confirm that the GPU runtime can load the exact configuration.
2. Run the candidate on `sample_test.wav` in an isolated job while observing free VRAM and the existing GPU process. If resources are insufficient, report the failure without stopping user processes. Record the model, configuration, timing, and peak memory if measured.
3. Verify four complete stems with matching sample rate, channels, and exact frame counts. Inspect leading and trailing samples and test alignment against the source. Compare all-on playback to the original and check all-off silence. Listen to each isolated stem and representative switch combinations.
4. Test a reference mixture with known ground-truth stems if a suitable licensed fixture is available. The supplied WAV alone cannot establish source SDR because its original component stems were not provided.
5. Exercise connection failure, expired login, partial transfer, checksum mismatch, stale result after sample replacement, repeated requests, multiple plug-in instances, host closure, project restore, and sample rates other than 44.1 kHz. Confirm that audio playback stays responsive throughout.

These are remaining implementation and acceptance checks. The investigation did not install packages, upload the test sample, start inference, or measure separation quality.
