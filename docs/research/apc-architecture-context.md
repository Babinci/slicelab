# SliceLab architecture and Linux build context

Investigation date: 2026-11-01. This is a source and machine inspection, not a successful build or a listening test. The checked-out Audio Plugin Coder (APC) reports version 1.8.0. Its Git submodule is pinned at `a1852d2`.

## Recommendation

Use APC's WebView path for the first integrated editor, with a JUCE instrument and DSP core that does not depend on the editor. This is the APC-supported option most directly suited to SliceLab's large, interactive waveform and original visual design. Fix its Linux-specific build and backend settings before relying on its template. Treat drag-and-drop, keyboard behavior, scaling, and REAPER embedding as things to verify in a running host, not as consequences of compiling.

Use Rubber Band as the first time-stretch and pitch-shift candidate, with a replaceable engine boundary. Its local runtime already provides the necessary API, but the development headers are absent. License terms and audible quality at the required settings remain decisions to check before distribution.

Native JUCE is the strongest fallback if the embedded WebView proves unreliable in REAPER. It avoids a browser runtime and provides direct pointer, file-drag, and waveform drawing integration. APC's plan and implementation workflows currently recognize only `webview` and `visage`, so native JUCE is an explicit workflow exception rather than a third APC-supported framework. See [`plan/SKILL.md`](../../vendor/audio-plugin-coder/.agents/skills/plan/SKILL.md#L161) and [`impl/SKILL.md`](../../vendor/audio-plugin-coder/.agents/skills/impl/SKILL.md#L159).

## The machine and framework are not ready to build

`bash scripts/system-check.sh --check-all` from the APC root reports Ubuntu 24.04.4 on x86-64, GCC 13.3, Node 22.23.2, and Python 3.12.3. It reports no CMake, initialized JUCE, EGL development files, or WebKitGTK development files. Independent `pkg-config` probes also could not find ALSA, FreeType, Fontconfig, X11, X extensions, GTK 3, or Rubber Band development packages. These are development checks. Runtime libraries alone do not supply their headers or CMake metadata.

The nested JUCE, Visage, pluginval, and CLAP submodules are all uninitialized. An empty `_tools/JUCE` directory exists, so the existence check in [APC's root CMake file](../../vendor/audio-plugin-coder/CMakeLists.txt#L64) is insufficient to prove the checkout is usable. APC's config is also absent. Its [setup instructions](../../vendor/audio-plugin-coder/.agents/skills/apc-setup/SKILL.md#L143) describe a soft gate when setup is incomplete. Use APC's setup procedure and path helpers before the plugin phases.

The first build also needs a Linux builder. Despite APC's [agent guidance](../../vendor/audio-plugin-coder/AGENTS.md) describing the shell builder as applicable to Linux, [`scripts/build-and-install.sh`](../../vendor/audio-plugin-coder/scripts/build-and-install.sh#L78) hard-codes the Xcode generator and macOS architecture options, attempts AU, and installs to `~/Library/Audio/Plug-Ins`. [`node bin/apc.js build`](../../vendor/audio-plugin-coder/bin/apc.js#L253) dispatches to that same shell script on Linux. Therefore installing packages alone will not make the advertised command work. Repair or replace the Linux branch of that wrapper in the project-owned build path, keeping the APC no-install build option and configured paths. Validate the actual Linux command after the toolchain exists.

APC's [Linux dependency list](../../vendor/audio-plugin-coder/docs/build-system.md#L553) names CMake, ALSA, FreeType, X11 and related extensions, EGL, WebKitGTK, JACK, and xvfb for its full feature set. The chosen editor and formats determine which packages are needed. Start with VST3, add optional formats only when required, and avoid an automatic install during build.

## Editor choices

| Architecture | Strengths for SliceLab | Work and risk |
| --- | --- | --- |
| JUCE plus WebView | APC has a UI map, components, resize rules, parameter relays, and an HTML preview path. Canvas can draw a detailed waveform and markers from native peak data. | Linux needs WebKitGTK and a native-to-JavaScript bridge. The shipped WebView CMake template asks for Windows WebView2 and omits Linux's `NEEDS_WEB_BROWSER TRUE`. Its editor requests the Windows backend unconditionally. Test Linux initialization and file drops in REAPER. |
| JUCE plus Visage | APC officially supports it. The UI is C++ and does not need WebKitGTK. Its template has a root frame and resize callback. | APC's shared adapter defaults to a windowless render, screenshot readback, and JUCE blit. It forwards mouse down, drag, up, and move, but the inspected adapter has no wheel, keyboard, or file-drop forwarding. Those interactions need a tested implementation for a sampler. Frame rate and raster quality can change under load. |
| Native JUCE `Component` editor | Direct JUCE waveform rendering, file chooser, file-drop target, pointer and keyboard handling. No WebKitGTK or Visage renderer bridge is needed. A custom look and embedded assets can give it an original appearance. | APC's phase validators do not model this framework. The team must own layout, accessibility, control styling, and its verification, and document the APC workflow deviation. |

Evidence for these tradeoffs is in [the WebView template CMake file](../../vendor/audio-plugin-coder/templates/webview/CMakeLists.txt.template#L4), [the WebView editor template](../../vendor/audio-plugin-coder/templates/webview/PluginEditor.cpp.template#L23), [APC's Linux WebView rule](../../vendor/audio-plugin-coder/.agents/rules/juce-build-protocols.md#L58), [the Visage editor template](../../vendor/audio-plugin-coder/templates/visage/PluginEditor.cpp.template#L4), and [the Visage host](../../vendor/audio-plugin-coder/common/VisageJuceHost.h#L123). The Visage host's mouse dispatch and readback are visible at [mouse dispatch](../../vendor/audio-plugin-coder/common/VisageJuceHost.h#L382) and [render scheduling](../../vendor/audio-plugin-coder/common/VisageJuceHost.h#L194). No editor has been profiled on this machine.

For WebView, make the canvas a view of precomputed waveform peaks, slice boundaries, and a low-rate playhead. Keep audio buffers and DSP in C++. Send gestures and parameter changes over JUCE relays or an explicit message interface. The audio callback must never wait for the page. Decode files and generate waveform peaks on a worker; send bounded view data rather than full audio through the bridge. Use a native asynchronous chooser as a reliable import path, and test both external file drops and any REAPER media-item drag separately because their payloads may differ.

Author an original instrument layout with the waveform as the primary working area, clearly legible slice markers, and a bank of MIDI-triggered pads. Use typography, spacing, color, and contrast as a coherent design system rather than copying a commercial instrument's artwork. The APC [WebView layout contract](../../vendor/audio-plugin-coder/.agents/rules/ui-layout-system.md#L7) describes a scalable plate, aspect handling, device-pixel-ratio-aware canvas sizing, and an in-page resize grip. Verify the minimum size, large size, host-initiated resize, multiple displays, and actual pointer hit areas in REAPER.

For Linux, select WebKit's supported backend instead of forcing the Windows backend. Set `NEEDS_WEB_BROWSER TRUE` and `JUCE_WEB_BROWSER=1`, embed the UI resources, and follow APC's current relay lifetime order. The provided [WebView header](../../vendor/audio-plugin-coder/templates/webview/PluginEditor.h.template#L48) explains why relays outlive the browser and attachments. The template is a starting point, not a working cross-platform example as checked out.

## Instrument and thread boundaries

Model the source as immutable audio plus its sample rate and frame count. A slice references a source and an integer frame interval with `0 <= start < end <= frameCount`. A pad maps a MIDI note to a slice and its playback settings. Keep a versioned, serializable session model separate from the current voices and GUI view state.

On import, a worker reads and decodes the file, validates it, computes peaks and analysis, and publishes a new immutable snapshot. Each voice captures a valid snapshot and slice when triggered. Publish replacement snapshots without blocking the audio callback, and reclaim old audio data off the audio thread. Merely using a shared pointer is insufficient if releasing its last reference causes a large buffer to be destroyed in `processBlock`.

In `prepareToPlay`, allocate the fixed voice pool, scratch buffers, and stretch processors. In `processBlock`, clear the output, read host tempo when available, consume MIDI at its sample offsets, render active voices, and mix the stereo output. Handle zero-length and variable-size blocks, source and host sample-rate differences, note-off, voice stealing, and sample-boundary fades. Never read files, allocate, wait on a worker, or call the editor from the callback. APC states those real-time restrictions in its [implementation rules](../../vendor/audio-plugin-coder/.agents/skills/impl/SKILL.md#L505).

JUCE's `Synthesiser` can manage voice assignment, but its [source](https://github.com/juce-framework/JUCE/blob/master/modules/juce_audio_basics/synthesisers/juce_Synthesiser.h) declares a `CriticalSection` and a default 32-sample minimum MIDI rendering subdivision. Do not describe it as inherently lock-free or assume every MIDI event is sample-accurate at that default. A custom fixed voice pool can process each `MidiBuffer` event offset explicitly. Alternatively, prove the chosen JUCE synth use cannot contend and configure the desired subdivision, then verify event timing. JUCE's [processor documentation](https://docs.juce.com/master/classjuce_1_1AudioProcessor.html) says host blocks can vary and MIDI timestamps are offsets within the block.

Store stable host automation parameter identifiers separately from the larger slice and pad state. Save sample identity and location along with edits, define what happens when the sample is missing, and verify reopening a project. Do not assume an absolute path alone makes a project portable. Reading and relinking media must remain off the audio callback.

## Independent time and pitch

This machine has Ubuntu's `librubberband2` version `3.3.0+dfsg-2build1` at `/lib/x86_64-linux-gnu/librubberband.so.2`. `nm -D` confirms exported time-ratio, pitch-scale, process, retrieve, start-padding, start-delay, and maximum-process-size methods. No Rubber Band header or `rubberband` pkg-config module was found. Install or otherwise provide a pinned development package or source dependency before compiling against it. A runtime SONAME alone is not a reproducible development setup.

Rubber Band's [API overview](https://www.breakfastquay.com/rubberband/code-doc/) describes independent time and pitch ratios, offline and real-time operation, and R2 and R3 engines. For a constant detected source tempo and host tempo, a first model is `timeRatio = sourceBpm / hostBpm`; a pitch offset of `s` semitones uses `pitchScale = 2^(s/12)`. Source-to-host sample-rate conversion is a separate operation. When host tempo or source tempo is unavailable, use an explicit unsynchronized state or a user-specified tempo rather than inventing one.

Do not assume one input block produces one equal-sized output block. Rubber Band's [integration notes](https://breakfastquay.com/rubberband/integration.html) recommend a pull-oriented design that consults `available()` and `getSamplesRequired()` before `process()` and `retrieve()`. Allocate for a documented maximum processing size. Bound work per callback, and define the underrun behavior. Real-time mode requires start padding and delay handling; test note-on timing and report any actual host latency. Priming can be expensive, even when normal processing is real-time safe. Arbitrary live MIDI notes cannot rely on advance notice, so instant response, highest-quality stretch, and unrestricted polyphony are not yet demonstrated together.

Consider a bounded worker-built cache for common slice, pitch, and tempo combinations, with versioned invalidation when edits change. Define what the player hears while a render is pending, and never silently serve an obsolete version as if it reflected a new edit. Offline mode is not real-time safe and belongs on a worker. Measure R2 and R3 for audible transients, timing, CPU, memory, and multiple voices before choosing a default. The 4.0 online docs may include behavior newer than the installed 3.3 library, so pin and check the actual development headers used for the build.

The [Rubber Band license](https://breakfastquay.com/rubberband/license.html) is GPL version 2 or later, with a separate commercial license for proprietary distribution. Check the chosen dependency build's transitive licenses too. JUCE is separately [dual licensed](https://juce.com/get-juce/) under AGPLv3 or its JUCE license. This does not establish the intended product's license; resolve distribution terms before shipping.

## CMake and host contract sketch

This is a proposed plugin target inside a configured APC plugin directory, not a tested build file. APC's root already adds JUCE, so do not add the JUCE modules a second time. Replace the illustrative manufacturer and plugin codes with stable product identifiers before release.

```cmake
juce_add_plugin(SliceLab
	COMPANY_NAME "SliceLab publisher"
	PLUGIN_MANUFACTURER_CODE Slab
	PLUGIN_CODE Slb1
	PRODUCT_NAME "SliceLab"
	FORMATS VST3 Standalone
	IS_SYNTH TRUE
	NEEDS_MIDI_INPUT TRUE
	NEEDS_MIDI_OUTPUT FALSE
	IS_MIDI_EFFECT FALSE
	VST3_CATEGORIES Instrument Sampler
	NEEDS_WEB_BROWSER TRUE)

juce_add_binary_data(SliceLabWebAssets SOURCES Source/ui/public/index.html)
find_package(PkgConfig REQUIRED)
pkg_check_modules(RUBBERBAND REQUIRED IMPORTED_TARGET rubberband>=3.3)
target_sources(SliceLab PRIVATE Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_compile_definitions(SliceLab PUBLIC JUCE_WEB_BROWSER=1 JUCE_USE_CURL=0
	JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(SliceLab PRIVATE SliceLabWebAssets PkgConfig::RUBBERBAND
	juce::juce_audio_utils juce::juce_gui_extra juce::juce_dsp)
```

The target is an instrument with MIDI input and stereo audio output, not an insert effect. The processor must implement matching bus layout, MIDI acceptance, stable state, and host parameter contracts. JUCE documents the target arguments and VST3 categories in its [CMake API](https://github.com/juce-framework/JUCE/blob/master/docs/CMake%20API.md). If the project uses native JUCE instead, omit the browser flag, WebView resources, and browser module requirements. This sketch does not address non-Linux backend flags or packaging.

## REAPER validation

REAPER is installed at `/home/wojtek/opt/REAPER/reaper`. The local launcher `/home/wojtek/.local/bin/reaper-pipewire` requests 48 kHz with 128-frame production, 64-frame low-latency, and 512-frame mixing modes. The per-user `~/.vst3` directory already contains Vital, and REAPER's VST cache lists it as an instrument. This supports a Linux VST3 workflow, but it does not verify SliceLab until REAPER scans and loads the actual bundle.

Build without installation first, inspect the VST3 bundle and shared-library dependencies, then test installation to a configured per-user VST3 scan path. In REAPER, verify that the plugin appears as an instrument, receives MIDI, makes sound, restores state and samples after save and reopen, reports any latency, and survives editor open and close. Test resize, scale, file chooser, external file drop, waveform gestures, automation, transport start and stop, tempo changes, multiple voices, and multiple instances. Render an offline track and compare timing with playback. Test at the launcher's three buffer settings and with variable callback sizes. The nominal 128-frame interval at 48 kHz is 2.67 ms, a mathematical block duration rather than a measured CPU budget or performance result.

Throughput checkpoint: n/a, read-only investigation. The design recommendation applies Experience First by prioritizing the waveform and reliable instrument interactions over implementation convenience. The competing editor architectures remain candidates until the host tests settle their interaction and performance tradeoffs.
