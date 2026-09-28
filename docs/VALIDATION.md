# Compatibility And GPU Rendering Verification

Verified on 2026-09-28. These results cover the reported API rejection,
immediate plugin-load crash, and production GLES shader execution. They do not
cover the full real-compositor rendering release matrix.

## Package Baselines

- Noctalia `5.1.0-1`, upstream tag `v5.1.0`, commit
  `c7b9197af77ff22bfb9a83c52a95643a1d90ca86`.
- Hyprland `0.56.2-3`, host commit
  `efb50993780079460b0cbed1363e2166a2de1d9f`.
- Hyprgraphics `0.5.1-4`; Hyprutils `0.14.2`.

The packaged Noctalia is v5's native C++/GLES shell with Luau plugins, not v4's
Quickshell/QML shell. The previously stated QML wallpaper implementation was
incorrect for this baseline. The v5 wallpaper ownership and reuse findings are
in [NOCTALIA_WALLPAPER_REUSE.md](NOCTALIA_WALLPAPER_REUSE.md).

## Reproduced Failures And Fixes

### Noctalia API Compatibility

The old manifest declared API 32. Released v5.1.0 accepts APIs 3 through 30;
`noctalia.getColor()` (API 31) and container tooltips (API 32) belong to later
upstream development, not this release. References:

- [v5.1.0 compatibility constants](https://github.com/noctalia-dev/noctalia/blob/v5.1.0/src/scripting/plugin_api.h)
- [v5.1.0 API history](https://github.com/noctalia-dev/noctalia/blob/v5.1.0/docs/plugin-api.json)

The plugin now declares API 28, its highest required feature (native context
menus). Workspace tooltips use `onHover` and `barWidget.setTooltip()` instead
of container tooltip properties. Color-picker inputs are six-digit RGB;
theme-role colors remain valid previews without calling the unreleased API.

Native testing also reproduced a host closing-animation race: open the styles
panel, close it, and immediately disable the plugin. The host aborted with
`pure virtual method called`. Waiting for the close animation eliminated the
crash. In v5.1.0, `PanelManager::unregisterPanel()` calls `closePanel(false)` and
erases the panel; `closePanel()` returns early if an animated close is already
in progress, leaving that animation with a freed panel pointer. The debugger
confirmed `SIGABRT` through `__cxa_pure_virtual` on the host's main thread.

The styles panel now uses v5's persistent floating-panel lifecycle, whose close
path cancels animations before releasing its scene. It has explicit close
controls and does not dismiss on outside clicks. This host lacks `ui.select`
support in persistent panels, so left clicks expand inline choices; right
clicks can open native menus. Native menus require a right-click pointer
context and accept at most 64 items; larger lists remain selectable inline.

### Hyprland Load Crash

The packaged host loader reproduced a segmentation fault in
`MainThreadWake::MainThreadWake()` during `pluginInit()`. Hidden visibility
made the Hyprland header's inline compositor globals private to the module.
The plugin consequently dereferenced its own null `g_pCompositor`, rather
than the compositor's initialized instance.

The module now uses default visibility for host symbol interposition. GNU
builds disable GNU-unique symbols to permit unload. The symbol regression
checks that `g_pCompositor` and `g_pHyprRenderer` are exported and preemptible.
It failed on the original binary's local globals and passes on the fix.

## Automated Checks

From the repository root:

```bash
lua tests/run.lua
lua tests/runtime.lua
cmake -S hyprland -B /tmp/luxaxis-build -DCMAKE_BUILD_TYPE=Release \
  -DLUXAXIS_BUILD_LIFECYCLE_TEST=ON
cmake --build /tmp/luxaxis-build --parallel 4
ctest --test-dir /tmp/luxaxis-build --output-on-failure
dbus-run-session bash tests/noctalia_smoke.sh .
```

Results: 10 model tests, 14 runtime tests, and 8 CTest cases passed. The
runtime mock intentionally omits `getColor()`. It covers the RGB picker
contract, released hover tooltips, native and inline choices, and lists over
the native menu limit.

The optional lifecycle test uses the actual packaged Hyprland executable and
public plugin loader. Its preload probe stops Aquamarine before GPU startup,
then loads, invokes the reload and Spotlight commands, emits a workspace
event, and unloads Luxaxis three times. It uses real host globals; it does
not substitute dummy compositor objects. It intentionally exits before the
incomplete compositor's own teardown.

The Noctalia smoke test needs Sway, Noctalia, socat, grim, jq, a C compiler,
pkg-config, Wayland client development files, and wayland-scanner. Run it as
a non-root user. It creates an isolated headless Sway display, configuration,
state, cache, and plugin directory. It uses pixman and llvmpipe, with fixture
Hyprland JSON and a test dispatch log, not a live Hyprland session.

It verifies plugin discovery, service startup, rendered widget/panel, a real
mouse workspace dispatch, native and inline style selection, persisted JSON,
config reload with a panel open, and three disable/enable cycles including
disable while open and immediate disable after closing. It checks IPC replies,
service restart counts, process exit status, and native crash messages. Logs
and screenshots remain in the printed temporary artifact directory.

The virtual-pointer fixture is a reduced copy of the MIT-licensed
`wlr-virtual-pointer-unstable-v1` protocol; its copyright and license are in
the XML. Source: [wlrctl protocol](https://git.sr.ht/~brocellous/wlrctl/tree/c6bc60820bb8786c7509e651bcae9393a738f180/item/protocol/wlr-virtual-pointer-unstable-v1.xml).
The test helper uses generated Wayland bindings and keeps the device alive
so the host binds `wl_pointer` before receiving clicks.

## GPU Renderer

The Hyprland adapter now enqueues an opaque custom pass at
`RENDER_POST_WALLPAPER`. Its execution runs the production GLES 3 shader into
an RGBA8 sRGB texture, restores host GL state, and returns a standard Hyprland
texture pass for presentation and output transforms. No Noctalia library or
client EGL/Wayland loop is embedded.

Spotlight geometry and fade/wipe/grow/outer/clock transition geometry execute
in the shader. The CPU sends style parameters, not rasterized masks or tiled
clip regions. Uniform locations are cached. Static outputs reuse the composed
texture; cursor-only updates redraw the old/new effect bounds. Source image
decoding remains asynchronous and texture uploads remain on the host thread.

The dual-source design and fade/wipe/disc mathematics were adapted from
Noctalia v5.1.0 with MIT attribution in
[`noctalia-wallpaper.LICENSE`](../hyprland/third_party/noctalia-wallpaper.LICENSE).
No request queue is copied. Each workspace change replaces the destination;
mixed interrupted wallpaper is captured in GPU ping-pong targets before
Spotlight is applied. Live mask parameters keep following the current cursor.
Normal transitions sample the old image directly; matching wallpaper sampling
skips texture blending and snapshots.

The test uses an actual EGL GLES context, compiles/links the production shader,
draws it, and reads pixels back. The current `luxaxis-build-env` container has
neither `/dev/dri` nor `/dev/dxg`. Its renderer reports:

```text
GLES renderer: llvmpipe (LLVM 22.1.8, 256 bits)
GLES version: OpenGL ES 3.2 Mesa 26.2.3-arch1.1
```

Reproduce the verified container build and tests:

```bash
docker exec luxaxis-build-env cmake -S /src/hyprland \
  -B /tmp/luxaxis-gpu-build -DCMAKE_BUILD_TYPE=Release \
  -DLUXAXIS_BUILD_LIFECYCLE_TEST=ON
docker exec luxaxis-build-env cmake --build /tmp/luxaxis-gpu-build --parallel 4
docker exec luxaxis-build-env ctest --test-dir /tmp/luxaxis-gpu-build \
  --output-on-failure
```

`luxaxis_gpu_tests` covers all Spotlight types against the CPU geometry
reference, fit/focal-point/premultiplied-alpha sampling, image row orientation,
all five transitions and easing, repeated geometric and RGB fade interruptions,
same-type parameter interpolation, `none` mask interruption with Engine plans,
pending image replacement by the newest request, immediate Spotlight off/on
while loading, independent output renderers, fractional-scale partial redraws,
4K shader pixels, resize during active/pending transitions, full GL state
restoration, safe rejection of oversized targets, and bounded snapshot/mask
storage across 100 consecutive switches. These are offscreen assertions, not
hardware timing or live multi-monitor compositor tests.

Intermediate GPU targets are separate from the image LRU budget. Each output
owns one RGBA8 composition texture and at most two snapshot textures, released
at transition completion. At 3840x2160 this is about 31.64 MiB idle or 94.92 MiB
with two interruption snapshots, plus 64 bytes per retained mask style. These
are pixel-storage estimates, excluding driver allocation overhead and source
images. The 256 MiB setting limits cached source images, not total VRAM.

GPU compile/link/draw failures are logged and retain the last valid texture or
show the opaque fallback color. There is no silent CPU rendering fallback.
Uploads exceeding the driver's texture-size limit are rejected before calling
the native allocator. Plugin unload removes queued custom passes and releases
GPU resources with the host EGL context current.

For core-only builds without GLES/EGL test dependencies, set
`LUXAXIS_BUILD_PLUGIN=OFF` and `LUXAXIS_BUILD_GPU_TESTS=OFF`. To test shaders on
real hardware, run `luxaxis_gpu_tests` directly with `EGL_PLATFORM=surfaceless`
and without `LIBGL_ALWAYS_SOFTWARE=1`; verify its reported renderer. CTest
deliberately selects software rendering for reproducibility.

## Remaining Verification

The available environment has no GPU device. Software EGL now validates shader
execution and pixel behavior, including 4K, but not real-GPU speed. The
Hyprland lifecycle probe still exits before renderer startup. It does not
validate the custom-pass integration, native image uploads, rotation/mirroring,
hotplug, multi-monitor workspace movement, fullscreen damage scheduling, or
4K compositor performance. The real compositor matrix in
[HYPRLAND_PLUGIN.md](HYPRLAND_PLUGIN.md#verification-matrix) remains required
before claiming full rendering acceptance.

Updating existing installations requires replacing the complete Noctalia
manifest/scripts and rebuilding/replacing `luxaxis.so` against the target
machine's matching Hyprland headers. Updating configuration alone does not
repair either old artifact. See [README.md](../README.md).
