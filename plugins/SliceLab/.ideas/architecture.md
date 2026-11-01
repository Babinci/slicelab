# SliceLab architecture

The implementation contract is repository-root `docs/IMPLEMENTATION_CONTRACT.md`. Source boundaries and all state structures are defined there and in `Source/Domain.h`.

The processor adapts host MIDI and parameters to a fixed voice pool. An immutable decoded asset supplies source and stem PCM. A background owner loads, analyzes, separates, and exports media. The native editor reads snapshots and sends commands. Playback uses a prepared Rubber Band pull path and never waits for model, file, or GUI work.

The native editor and Linux build adapter extend the pinned APC framework deliberately. Repository-root `docs/linux-build.md` records the upstream limitations and the reproducible build path.
