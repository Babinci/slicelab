#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
command -v uv >/dev/null
command -v g++ >/dev/null
command -v pkg-config >/dev/null
if [[ -e .git ]]; then
    git submodule update --init vendor/audio-plugin-coder vendor/rubberband
    git -C vendor/audio-plugin-coder submodule update --init _tools/JUCE
else
    for source in vendor/audio-plugin-coder/CMakeLists.txt \
        vendor/audio-plugin-coder/_tools/JUCE/modules/juce_core/juce_core.h \
        vendor/rubberband/rubberband/RubberBandStretcher.h; do
        if [[ ! -f "$source" ]]; then
            printf 'Missing bundled source: %s\n' "$source" >&2
            exit 1
        fi
    done
fi
if [[ ! -x .toolchain/bin/cmake ]]; then
    uv venv .toolchain
    uv pip install --python .toolchain/bin/python 'cmake==4.4.4' 'ninja==1.13.2'
fi
packages=(libasound2-dev libfreetype-dev libfontconfig-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev libxrender-dev libxfixes-dev libgl-dev libglx-dev libxau-dev libxdmcp-dev libxcb1-dev x11proto-dev xtrans-dev libpng-dev libbrotli-dev libbz2-dev zlib1g-dev libexpat1-dev uuid-dev libfftw3-dev libsamplerate0-dev libxkbcommon-dev)
mkdir -p .deps/downloads
if [[ ! -f .deps/.headers-ready || ! -f .deps/usr/include/X11/extensions/XInput2.h ]]; then
    (cd .deps/downloads && apt-get download "${packages[@]}")
    python3 - "$root/.deps" <<'PY'
from pathlib import Path
import subprocess
import sys
prefix = Path(sys.argv[1])
for package in sorted((prefix / 'downloads').glob('*.deb')):
    subprocess.run(['dpkg-deb', '-x', str(package), str(prefix)], check=True)
for link in (prefix / 'usr/lib/x86_64-linux-gnu').glob('*.so'):
    if link.is_symlink() and not link.exists():
        runtime = Path('/usr/lib/x86_64-linux-gnu') / link.readlink()
        if runtime.exists():
            link.unlink()
            link.symlink_to(runtime)
(prefix / '.headers-ready').touch()
PY
fi
source scripts/env.sh
pkg-config --modversion alsa freetype2 fontconfig x11 xrandr xinerama xcursor fftw3 samplerate
printf 'SliceLab Linux toolchain is ready.\n'
