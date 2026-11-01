# DSP worker bridge checkpoint

This is a bounded design proposal from the source read on 2026-11-01. It is not approval to ship. The worker design below is a fallback candidate. The preferred next implementation is the bounded synchronous R3 scheduler described at the end. That scheduler avoids dependence on host callback cadence. Existing asset ownership, public processor commands, and the independent analysis worker remain the base.

The first blocking question is host scheduling. Reported latency grants sample-time delay, not wall-clock time. A host can submit callbacks faster than the nominal audio period while still reporting realtime mode. A finite asynchronous queue then underruns even when average DSP throughput exceeds real time. REAPER's actual anticipative, live-monitor, and playback scheduling must pass before this becomes the final architecture. Raising latency alone does not prove that premise.

The other blockers are sustained 32-voice throughput, exact view capture, and exclusive engine ownership during mode changes. A worker permits allocation outside the callback. It does not reduce computation or guarantee Linux scheduling deadlines. The [Rubber Band review](astra-engine-review.md) and [Signalsmith investigation](signalsmith-candidate.md) establish reasons to test this boundary, not its performance.

## One owner, two execution modes

`SamplerEngine` remains owned by `SliceLabAudioProcessor::Impl`. Only the DSP thread calls its processing methods in realtime mode. The offline caller acquires exclusive ownership after that thread drains its requests and parks. Publication and retirement retain their current non-realtime owners and mutex. The analysis worker never executes DSP jobs.

These are the proposed public methods of the private bridge class. Existing public processor commands and `Snapshot` stay unchanged.

```cpp
struct DspBridgeSpec {
	double sampleRate;
	uint32_t maxRealtimeFrames, latencyFrames;
};
enum class UiKind : uint8_t { Pad, Note, StopAll, Preview, StopPreview };
struct UiCommand {
	UiKind kind;
	int32_t index;
	float velocity;
	bool noteOn;
	double sourceSeconds;
};
struct PlaybackTelemetry {
	double playheadSeconds;
	std::array<float, padCount> activity;
	float peak;
	bool previewPlaying, voicesActive;
};
enum class BridgeFault : uint8_t {
	None, Deadline, RequestFull, MidiFull, UiFull, InvalidBlock, ViewContention
};
struct DspBridgeStats {
	uint64_t underruns, requestOverflows, midiOverflows, uiOverflows;
	uint64_t invalidBlocks, droppedEvents, maxPendingFrames, maxWorkerNanoseconds;
	BridgeFault lastFault;
};
class DspBridge {
public:
	explicit DspBridge(SamplerEngine&);
	~DspBridge();
	void prepare(const DspBridgeSpec&); // Stopped, outside processBlock.
	void release(); // Callbacks quiesced. Joins the DSP thread.
	void requestNonRealtime(bool) noexcept;
	void processRealtime(juce::AudioBuffer<float>&, const juce::MidiBuffer&,
		const PlaybackParameters&, const std::array<int, padCount>&,
		uint32_t enabledBusMask) noexcept;
	void processOffline(juce::AudioBuffer<float>&, const juce::MidiBuffer&,
		const PlaybackParameters&, const std::array<int, padCount>&,
		uint32_t enabledBusMask);
	bool enqueueUi(const UiCommand&) noexcept; // One message-thread producer.
	DspBridgeStats stats() const noexcept;
	PlaybackTelemetry audibleState() const noexcept;
	int latencySamples() const noexcept;
};
```

The processor snapshots host BPM, `PlaybackParameters`, output offsets, and the enabled bus mask once per host callback. Its realtime branch calls only `processRealtime` and clears MIDI. It performs no engine processing, ownership transfer, allocation, lock, wait, notification syscall, or asset reference-count operation. An override of `setNonRealtime(bool) noexcept` calls the JUCE base and `requestNonRealtime`. The callback also selects its branch from `isNonRealtime()`.

## Packet storage and publication

Use a request descriptor ring, a MIDI event ring, a forwarded UI command ring, an output descriptor ring, and a planar PCM frame ring. All storage is allocated and touched during `prepare`. A descriptor never contains a `shared_ptr`, `String`, vector, host buffer pointer, or borrowed MIDI pointer.

```cpp
struct MidiEvent { uint32_t offset; uint8_t size, bytes[3]; };
struct Request {
	uint64_t epoch, sequence, sourceFrame, midiBegin, uiBegin;
	uint32_t frames, channels, midiCount, uiCount, enabledBusMask;
	PlaybackParameters parameters;
	std::array<int, padCount> outputOffsets;
	const PlaybackView* view; // Opaque, protected by this slot's hazard.
};
struct Output {
	uint64_t epoch, sequence, sourceFrame, pcmBegin;
	uint32_t frames, channels, enabledBusMask;
	PlaybackTelemetry telemetry;
};
```

Let `M` be the supported maximum realtime callback size and `L` the reported latency. One normal host callback creates one request, preserving existing engine call boundaries. Descriptor capacities are `nextPow2(L + M + 2)`, which also supports one-frame callbacks. The planar PCM ring has 64 channels and `nextPow2(L + 2*M + 1)` frames. Do not allocate a separate `64*M` audio buffer for every descriptor. The DSP owner has one prepared `64*M` scratch buffer.

Use 65,536 MIDI entries, a 256-entry message-thread UI queue, and 4,096 forwarded UI entries initially. These are explicit bounded capacities, not a promise to accept arbitrary event density. A request accepts at most 4,096 MIDI records. Unsupported system messages do not copy their payload. Capacity failure rejects the whole request and starts fault recovery, including when the omitted event would have been note-off.

Every ring has one producer and one consumer, with monotonically increasing `uint64_t` read and write counters. Producers reserve all payload space before release-publishing a descriptor. Consumers acquire that descriptor, finish every payload read, then release the read counters. Slot states are therefore free, producer-owned, published, consumer-owned, and free again. Separate cache lines hold producer and consumer counters. Assert that the target's required pointer, integer, and meter atomics are lock-free.

In realtime mode the audio callback produces requests, MIDI, and forwarded UI entries. The DSP thread consumes them and produces output descriptors and PCM. The callback alone consumes output, retaining a partial-packet cursor across variable host blocks. Offline mode transfers these roles only after the DSP owner parks. Never reset live ring indices from the opposite actor.

The worker may poll an atomic request counter and sleep briefly on its own thread when idle. Its measured wake delay belongs in the latency budget. The callback does not use a condition variable, semaphore, `atomic::notify_one`, thread launch, or scheduler call to wake it. Thread priority is a measured implementation choice, not a deadline guarantee.

## Captured views retain the current ownership model

The current `SamplerEngine::process` calls `updateView()` at execution time. Leaving that unchanged would let queued notes use a later sample or slice edit. Likewise, leaving UI commands in its current queue would apply a recent preview command to old audio time.

Add an opaque forward declaration `struct PlaybackView` and the following engine helpers. The existing `process` remains available to independent synchronous engine users such as pad export.

```cpp
void attachRequestViewHazards(std::span<std::atomic<const PlaybackView*>>);
bool capturePublishedView(std::atomic<const PlaybackView*>&) const noexcept;
void processPinned(juce::AudioBuffer<float>&, const juce::MidiBuffer&,
	const PlaybackParameters&, const std::array<int, padCount>&,
	const PlaybackView*);
void detachRequestViewHazards();
```

There is one atomic view hazard per request slot and one for an offline callback. Registration and removal take the existing view mutex outside processing. Capture loads the published pointer, stores it to the slot hazard, and validates the published pointer again, with sequentially consistent operations. Three unsuccessful attempts cause a visible fault. No pointer is dereferenced before successful validation.

`processPinned` adopts that exact view into `retained` and `audioView`, then runs the existing engine path without `updateView`. The slot hazard stays set until processing ends and the worker releases the request. The collector scans request hazards before `retained`, then voice hazards. Preserve the existing `acquiring`-before-`retained` order for the ordinary engine path. This prevents retirement from missing a handoff from request to retained view or active voice. Shared ownership and destruction still occur only on non-realtime threads.

The callback drains message-thread UI commands into the next request at offset zero. The DSP owner forwards those commands to the existing engine queue immediately before `processPinned`. UI commands precede offset-zero MIDI, as today. Preview, keyboard audition, stop, and pad triggers consequently share `L` samples of monitoring latency. Queue overflow uses the same fault recovery as MIDI loss.

## Sample time and reported latency

Use a monotonic stream sample counter, independent of host PPQ, transport time, loops, and seeks. A request at `S` produces source interval `[S,S+N)`. Return that PCM only in output interval `[S+L,S+L+N)`. Each callback reads exactly its required source interval, crossing packet boundaries as necessary. The first `L` output frames are defined startup silence.

Copy each MIDI event's raw bytes and clamped sample offset, preserving event order and the existing end-of-block event semantics. A MIDI event at `S+offset` affects output at `S+offset+L`. Capture BPM and all playback parameters with the request. The DSP owner never rereads current APVTS values, current host BPM, or the playhead. This preserves current block-level automation. It does not invent sample-accurate VST3 automation that the present processor does not receive.

The output packet carries the original bus mask. A changed layout invalidates queued output even when channel counts match. Valid host layout changes reprepare the bridge. Update UI playback telemetry only as its output packet becomes audible. Compute output peak from the actual returned buffer. The worker's newest voice state would otherwise lead the audio by approximately `L`.

Call `setLatencySamples(L)` during `prepareToPlay`, with the same `L` in realtime and offline modes. Include the delay in tail handling and verification renders. Do not remove the physical delay in offline mode while leaving PDC enabled. Drain the final `L` frames with subsequent silent-input callbacks.

There is no measured shipping latency yet. A development candidate is `nextPow2(max(M+1, ceil(sampleRate*2048/48000)))`. Compare a 1024-frame base, a 2048-frame base, and larger candidates at every supported rate. For callbacks paced at the nominal sample rate, the conservative budget is `(L-M)/sampleRate`. The measured startup burst, scheduler delay, and backlog must fit that budget. Average sustained DSP consumption must also remain below production of audio frames. Choose the smallest candidate that passes the actual processor and REAPER stress gates. Never derive a shipping default from the Signalsmith single-voice measurements.

## Lifecycle and exceptional blocks

Use atomic desired mode `Realtime` or `Offline`, plus engine owner state `Stopped`, `Worker`, `OfflineIdle`, or `OfflineBusy`. Callbacks for one processor must remain serialized, as the current engine already requires.

`prepareToPlay` first stops the previous worker while callbacks are quiesced. It prepares the engine and storage, registers hazards, clears telemetry and queues, selects fixed latency, and starts the worker. Sample-rate changes require this full sequence. The sample asset keeps its original source rate. A sample-rate change never reuses queued PCM.

On realtime-to-offline transition, the first offline callback requests offline mode and may wait while the worker completes all published requests. The worker release-publishes `OfflineIdle` only after its current call ends and the request ring is empty. The offline caller acquires `OfflineBusy`, runs the same request execution method synchronously, and release-publishes `OfflineIdle` at callback end. Preserve engine voices, sample counter, queued output, and physical delay through this transition. Output capacity must hold the complete delayed interval so parking cannot deadlock on a full output ring.

On offline-to-realtime transition, the worker acquires `OfflineIdle` with compare-and-exchange after seeing the desired realtime mode. The realtime callback only publishes requests and consumes previously delayed output. It never waits for ownership or processes a fallback engine. `setNonRealtime` itself only changes flags. It never joins or drains a worker, because its caller's thread context is not a safe assumption.

`releaseResources` and destruction occur with callbacks quiesced. Request stop, join the DSP thread, release engine voices, drain pins and queues, and unregister hazards before freeing storage. The analysis worker must also finish before the engine and its publication state are destroyed. Mode changes alone do not reset the engine or change latency.

Zero-frame callbacks do not advance the stream counter. With MIDI or UI commands, they publish an ordered zero-frame request that changes engine state without producing PCM. Empty zero-frame callbacks do nothing. Repeated zero-frame traffic still has bounded queue capacity.

For realtime `N > M`, clear the buffer, record `InvalidBlock`, and enter fault recovery. Do not resize or secretly increase latency. Persistent oversize traffic requires host reprepare with a larger supported limit. No finite fixed latency can asynchronously satisfy arbitrary unbounded blocks. Offline callbacks can exceed `M`. Split them on the caller, retaining one captured view and parameter snapshot, rebasing MIDI offsets, and consuming delayed output between chunks.

## Failure cannot replay stale notes

An output deadline miss, missing timestamp range, exhausted queue, view capture failure, or invalid configuration latches a fault and advances an atomic recovery epoch. Clear the current callback and reject the rest of that epoch. Keep counters and a visible `Snapshot.error`, such as "DSP overload. Playback reset; retrigger held notes." Do not repeat the previous buffer, play a late packet, drop only note-offs, or run Rubber Band on the callback as a fallback.

The worker finishes its current call, discards remaining old requests while releasing their payload and view pins, stops voices and clears sustain and note ledgers, then acknowledges the new epoch. No further old output may be published after that acknowledgement. Until acknowledgement, the callback emits silence and counts discarded events. Once acknowledged, it advances its own output read counters past all old descriptors and PCM and sets the recovery start to the current stream frame. It accepts new requests with another `L` startup frames. Stream time and cursors remain monotonic. The worker never edits the callback's read cursors. Held notes are not automatically recreated.

Fault state and recovery counts remain visible after recovery. A successful stress test requires zero faults after deliberate startup, not merely plausible audio. Worker duration and queue backlog telemetry must be available without file logging or string creation on the callback.

## Gates before integration approval

1. Instrument actual processor callback cadence in real REAPER playback, record-arm monitoring, anticipative processing, stop/start, and offline rendering. If sample time outruns available worker wall time, stop this architecture decision before expanding implementation.
2. Run positive-controlled malloc, calloc, realloc, free, new, and delete hooks on the actual realtime `SliceLabAudioProcessor::processBlock`. Exercise note starts, extreme dynamic ratios and pitches, all 32 voices, publication, UI traffic, and bus layouts. Count worker allocations separately.
3. Measure worker startup bursts, sustained rendering, scheduler gaps, queue high-water marks, and underruns at 44.1, 48, 96, and 192 kHz. Include 64-, 128-, 512-frame, variable, and one-frame callbacks, 64 outputs, loops, stem changes, and competing analysis work. Measure wall time as well as thread CPU time.
4. Compare synchronous and paced asynchronous PCM for identical timestamped requests after removing exactly `L` leading frames. Preserve the tail. Check MIDI offsets, note ledgers, tempo steps, tuning automation, UI preview, source replacement, and transitions between modes. Different host block partitions can already change existing DSP behavior, so distinguish that from bridge timing errors.
5. Force request, MIDI, UI, and output failures and a parked worker. Prove bounded callback completion, reported faults, no stale output, no hanging held notes, and correct recovery. Exercise zero and oversized callbacks and layout and sample-rate changes.
6. Run deterministic hazard promotion schedules under ASan and ownership/publication stress under TSan. Include publication while requests wait, while voices retain old assets, and during teardown.
7. Build the VST3 and verify cold REAPER save/reload, host tempo automation, all outputs, realtime monitoring, and offline PCM with PDC. Reopen a fresh host. Existing pre-bridge neutral render equality does not close this gate.

The simpler safe fallback is a single DSP worker in every mode, with the offline caller waiting for exact packets. It removes engine ownership handoff but still needs the same realtime scheduling gate. If REAPER bursts realtime-mode callbacks, the robust alternative is a callback-safe renderer with verified bounded startup and allocation behavior. Do not silently select Repitch or substitute the rejected Signalsmith adapter. A host-specific no-anticipation requirement is a product constraint that must be explicit if the bridge depends on it.

## Preferred next candidate, bounded synchronous R3 work

The recorded [pitch probe](../../local/verification/rubberband-pitch-quality.py) has four cases and three windows per engine. Its [results](../../local/verification/rubberband-pitch-quality.json) show R3 peak-bin errors below 0.01 cents and R2-short errors up to 25.04 cents. The FFT grid is 0.09155 Hz, so this does not establish absolute pitch accuracy to 0.01 cents. It supports testing R3 before adding a worker dependency.

The root investigation identified reset-time queue replacement in `src/finer/BinClassifier.h`. Its proposed source patch reuses and zeroes existing entries instead of freeing and allocating them. This pass has not reviewed that source patch. The earlier allocation probe omitted `posix_memalign`; its zero malloc count was not proof of zero allocation. Audit aligned allocation and deallocation too, then repeat reset, setters, and processing across subsequent transitions. Prewarming alone is insufficient evidence.

The recorded [startup timings](../../local/verification/r3-startup.log) still exceed small callback budgets. A patched reset therefore needs bounded processing as well. Implement a resumable engine job on the audio thread, using the same timestamped request journal, captured views, parameters, MIDI, and output delay defined above. Remove the DSP thread and ownership handoff. Only view publication and UI input remain concurrent. The scheduler can be private to `SamplerEngine` with these additive integration methods:

```cpp
void prepareScheduled(double sampleRate, int maximumBlockSize, int latencyFrames);
void processScheduled(juce::AudioBuffer<float>&, const juce::MidiBuffer&,
	const PlaybackParameters&, const std::array<int, padCount>&, bool nonRealtime);
int scheduledLatencySamples() const noexcept;
uint64_t scheduledOverloads() const noexcept;
```

Retain ordinary `prepare` and `process` for synchronous export and existing engine tests. The root can switch the processor to the scheduled methods and report its latency separately. Terra can implement engine files and scheduler tests without editing processor or editor files.

Each callback first captures its entire control interval. It then advances the oldest unfinished render request using one global work allowance, before reading source interval `[T-L,T-L+N)`. A job retains its request slot and view hazard until all voices have mixed that interval. It records the MIDI cursor, current segment end, voice index, sample cursor, pending generated frames, padding count, and remaining stretcher delay. No engine event or parameter from a later request is applied early.

One microstep performs at most one measured bounded operation, such as one R3 input call with a fixed maximum chunk, one bounded retrieval, one bounded mix, or one voice configuration. Replace the pull-until-output loop with resumable phases. A step never recursively primes a note to completion. Include reset, setters, filters, metering, and event dispatch in the work bound. A fixed input size is not proof that R3 performs bounded small work inside that call; measure its worst indivisible call, and split the library operation if necessary.

Set the global allowance from host frames and measured operation costs, with a per-callback cap. Never grant the full allowance independently to each of 32 voices. An initially conservative 128-frame feed quantum is a benchmark candidate, not an approved bound. Keep fractional work credits for variable blocks, but do not accumulate unlimited idle credits and spend them in one strike callback. One-frame callbacks remain a correctness case; a hard deadline claim must identify the measured minimum supported hardware block size.

Discard each voice's R3 startup padding and delay exactly once. The first retained voice frame has the note's logical source timestamp and becomes audible at that timestamp plus `L`. Render no externally controlled output interval beyond the latest captured control horizon. Prerecorded source lookahead may extend farther. Direct and Repitch voices use the same delayed output timeline. A per-voice countdown that changes when host block sizes change is insufficient.

Apply note-off, sustain, all-notes-off, voice stealing, envelopes, and end-of-slice at their logical request positions. A note-off received while startup remains pending must not destroy that note's earlier, unrendered audio. Ordered jobs preserve this rule without a second immediate note path. Finished voice PCM remains in delayed output until consumed, even after the engine voice becomes inactive. Looping voices have no premature final marker. Retain the existing note sequence ledger and view hazards.

Offline mode runs the identical jobs synchronously to completion while retaining the same physical `L` delay. It needs no ownership exchange or special parameter reads. Candidate delays of 4096 and 8192 frames require rate-specific measurements. A simultaneous 32-voice startup must fit the total work available before its first due output, and sustained work must fit every later interval. The single-voice startup logs do not establish either condition. Benchmark 128- and 512-frame production buffers separately.

If due PCM is incomplete, emit silence, increment the overload counter, stop voices and note ledgers, discard the incomplete epoch, and expose the reset through `Snapshot.error`. Restart with `L` startup frames. Do not silently lower polyphony, switch to Repitch, lose note-offs, emit partial voice mixes, or replay stale audio. Repeated overload is a failed acceptance gate, not an acceptable steady-state quality mode.

Laziness Protocol favors the single-thread scheduler over the worker when R3 can meet its operation budget. Separate Before Serializing Shared State keeps one DSP owner. Model the Domain makes resumable jobs and recovery explicit states. Prove It Works requires allocator, deadline, and host PCM evidence. This bounded design pass ran no builds or performance experiments.
