#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
python3 - "$root" <<'PY'
from datetime import datetime, timezone
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import sys
import tarfile
import tempfile

root = Path(sys.argv[1])
bundle = root / 'build/plugins/SliceLab/SliceLab_artefacts/Release/VST3/SliceLab.vst3'
binary = bundle / 'Contents/x86_64-linux/SliceLab.so'
if not binary.is_file():
    raise SystemExit('Build and verify SliceLab before packaging')
dirty = subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=normal'], cwd=root, text=True).strip()
if dirty:
    raise SystemExit('Commit the complete audited source before packaging')
version = '0.1.0'
destination = root / 'release' / f'SliceLab-{version}-linux-x86_64'
if destination.exists():
    raise SystemExit(f'Package directory already exists: {destination}')
destination.mkdir(parents=True)
shutil.copytree(bundle, destination / bundle.name)
for name in ('LICENSE', 'NOTICE.md', 'README.md'):
    shutil.copy2(root / name, destination / name)
shutil.copytree(root / 'docs', destination / 'docs')
(destination / 'licenses').mkdir()
for name, path in (
    ('APC-LICENSE.md', 'vendor/audio-plugin-coder/LICENSE.md'),
    ('JUCE-LICENSE.md', 'vendor/audio-plugin-coder/_tools/JUCE/LICENSE.md'),
    ('RubberBand-COPYING', 'vendor/rubberband/COPYING'),
):
    shutil.copy2(root / path, destination / 'licenses' / name)
(destination / 'workers').mkdir()
for name in ('stem_worker.py', 'remote_worker.py'):
    shutil.copy2(root / 'scripts' / name, destination / 'workers' / name)
revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
source_archive = destination.parent / f'SliceLab-{version}-source.tar.gz'
source_prefix = f'SliceLab-{version}-source/'
source_repositories = (
    ('', root),
    ('vendor/audio-plugin-coder/', root / 'vendor/audio-plugin-coder'),
    ('vendor/audio-plugin-coder/_tools/JUCE/', root / 'vendor/audio-plugin-coder/_tools/JUCE'),
    ('vendor/rubberband/', root / 'vendor/rubberband'),
)
with tempfile.TemporaryDirectory(prefix='slicelab-source-') as temporary:
    with tarfile.open(source_archive, 'w:gz') as output:
        for index, (prefix, repository) in enumerate(source_repositories):
            archive_path = Path(temporary) / f'{index}.tar'
            with archive_path.open('wb') as stream:
                subprocess.run(['git', 'archive', '--format=tar', 'HEAD'], cwd=repository, stdout=stream, check=True)
            with tarfile.open(archive_path) as archive:
                for member in archive:
                    member.name = source_prefix + prefix + member.name
                    content = archive.extractfile(member) if member.isfile() else None
                    output.addfile(member, content)
manifest = {
    'name': 'SliceLab', 'version': version, 'platform': 'linux-x86_64',
    'created_utc': datetime.now(timezone.utc).isoformat(),
    'source_url': 'https://github.com/Babinci/slicelab', 'source_revision': revision,
    'source_worktree_dirty': bool(dirty),
    'corresponding_source_archive': source_archive.name,
    'corresponding_source_sha256': hashlib.sha256(source_archive.read_bytes()).hexdigest(),
    'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
    'dependencies': subprocess.check_output(['git', 'submodule', 'status'], cwd=root, text=True).strip().splitlines(),
}
(destination / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
(destination / 'RUNTIME_DEPENDENCIES.txt').write_text(subprocess.check_output(['ldd', str(binary)], text=True))
instructions = '''Copy SliceLab.vst3 to ~/.vst3/ and rescan VST3 instruments in REAPER.
Copy both workers/*.py files to ~/.local/share/slicelab/ to use stem separation.
The remote inference worker must already be installed and authenticated; see the source repository.
Read docs/user-guide.md and the source repository's verification report before testing.
'''
(destination / 'INSTALL.txt').write_text(instructions)
checksums = []
for path in sorted(destination.rglob('*')):
    if path.is_file():
        checksums.append(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(destination)}')
(destination / 'SHA256SUMS').write_text('\n'.join(checksums) + '\n')
archive = destination.parent / (destination.name + '.tar.gz')
with tarfile.open(archive, 'w:gz') as output:
    output.add(destination, arcname=destination.name)
print(archive)
print(source_archive)
PY
