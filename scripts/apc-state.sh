#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root/vendor/audio-plugin-coder"
source scripts/state-management.sh
STATE_FRAMEWORKS+=(native-juce)
update_plugin_state "$root/plugins/SliceLab" "$@"
