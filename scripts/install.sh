#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
python3 - "$root" <<'PY'
from datetime import datetime, timezone
from pathlib import Path
import os
import shutil
import sys
import tempfile

root = Path(sys.argv[1])
source = root / 'build/plugins/SliceLab/SliceLab_artefacts/Release/VST3/SliceLab.vst3'
if not source.is_dir():
    raise SystemExit('Build SliceLab first with bash scripts/build.sh')
install = Path.home() / '.vst3'
install.mkdir(parents=True, exist_ok=True)
destination = install / source.name
temporary = Path(tempfile.mkdtemp(prefix='.slicelab-install-', dir=install))
staged = temporary / source.name
try:
    shutil.copytree(source, staged)
    if destination.exists():
        backups = Path.home() / '.cache/slicelab/install-backups'
        backups.mkdir(parents=True, exist_ok=True)
        stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
        os.rename(destination, backups / (stamp + '.vst3'))
    os.rename(staged, destination)
finally:
    shutil.rmtree(temporary)
worker_dir = Path.home() / '.local/share/slicelab'
worker_dir.mkdir(parents=True, exist_ok=True)
for name in ('stem_worker.py', 'remote_worker.py'):
    shutil.copy2(root / 'scripts' / name, worker_dir / name)
print('Installed VST3:', destination)
print('Installed stem client:', worker_dir / 'stem_worker.py')
PY
