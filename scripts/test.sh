#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
bash scripts/build.sh SliceLabEngineTests SliceLabProcessorFeatureTests \
    SliceLabScheduledEngineTests SliceLabRubberBandAllocations SliceLabProcessorAllocations SliceLabMarkerMetronomeTests
mkdir -p local/verification
SLICELAB_KEY_ANALYZER=off build/plugins/SliceLab/SliceLabEngineTests_artefacts/Release/SliceLabEngineTests \
    > local/verification/engine-tests.log 2>&1
SLICELAB_VERIFY_MEMORY="${SLICELAB_VERIFY_MEMORY:-1}" \
    build/plugins/SliceLab/SliceLabProcessorFeatureTests_artefacts/Release/SliceLabProcessorFeatureTests \
    > local/verification/processor-feature-tests.log 2>&1
build/plugins/SliceLab/SliceLabScheduledEngineTests > local/verification/scheduled-engine-tests.log 2>&1
build/plugins/SliceLab/SliceLabRubberBandAllocations > local/verification/rubberband-allocations.log 2>&1
build/plugins/SliceLab/SliceLabProcessorAllocations > local/verification/processor-allocations.log 2>&1
build/plugins/SliceLab/SliceLabMarkerMetronomeTests > local/verification/marker-metronome-tests.log 2>&1
.toolchain/bin/python -m unittest discover -s tests -p 'test_*.py' > local/verification/python-tests.log 2>&1
printf 'All test programs passed. Logs: %s/local/verification\n' "$root"
