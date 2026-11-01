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
