# Linux build adapter

SliceLab builds through Audio Plugin Coder's root CMake project and its pinned JUCE submodule. The plugin lives in the configured external `plugins` directory.

The pinned APC shell builder hard-codes the Xcode generator, universal macOS flags, AudioUnit, and macOS install directories. `scripts/build.sh` is the Linux adapter for that missing path. It uses Ninja, disables unused Visage and CLAP dependencies, and produces Linux VST3 and Standalone targets. It does not modify the pinned framework.

The native JUCE editor is a deliberate extension of APC's WebView and Visage choices. Native JUCE supplies Linux keyboard, wheel, file drop, accessibility, and host window behavior directly. The engine and GUI remain normal JUCE plugin modules under APC's build project.

Run these commands from the repository root:

```bash
bash scripts/bootstrap-linux.sh
bash scripts/build.sh
```

Bootstrap places CMake, Ninja, and Ubuntu development packages in ignored repository-local directories. It does not require administrator privileges. The runtime libraries come from the host. On other Linux distributions, install the equivalent development packages and adapt `scripts/env.sh` to their locations.


Rubber Band 3.3.0 is a second pinned submodule and is built statically. The CMake adapter applies one exact-match reset-buffer reuse change to a build-directory copy; the vendor checkout stays unchanged. FFTW3 (double precision) and libsamplerate remain host runtime dependencies. A mismatched upstream patch fails configuration.

The tested local tools are GCC 13, CMake 4.4.4, and Ninja 1.13.2. Bootstrap pins the latter two; development packages are resolved from the machine's configured Ubuntu package repositories and are not fully version-pinned. The default compiler concurrency is two jobs, including JUCE's configure-time helper. Set `SLICELAB_BUILD_JOBS` to change it.

`bash scripts/test.sh` runs behavior and allocation checks. The optional Address/UBSan and ThreadSanitizer concurrency targets are excluded from normal builds. Their coverage and any host-specific invocation requirements are recorded in the verification reports; a partially instrumented dependency graph is insufficient to dismiss a race report.


The corresponding-source release archive includes APC, its required JUCE tree, and Rubber Band. It can be built after extraction without Git metadata: bootstrap validates those bundled source files and skips submodule commands. A Git checkout instead initializes the pinned submodules normally. The binary package and source archive are paired by SHA-256 in the package manifest.
