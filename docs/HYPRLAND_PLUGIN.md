# Luxaxis Hyprland Plugin Specification

Status: accepted design; implementation starts after the documentation-only
commit containing this file.

This is the source of truth for Phase 2. A new development session must read
this file before changing the Hyprland plugin. Phase 1 is the existing
Noctalia workspace widget at repository commit `282b2ba`.

## Objective

Build an independently usable Hyprland plugin that:

1. Binds normal numeric workspaces to different wallpapers.
2. Switches the wallpaper on the affected output when its active workspace
   changes.
3. Applies a cursor-driven Spotlight effect to the wallpaper only.
4. Allows each workspace profile to select a different Spotlight.
5. Later adds optional wallpaper transitions without redesigning the renderer.

The Noctalia bar widget and the Hyprland plugin share a product repository but
have no runtime dependency. A Noctalia settings frontend and its control
protocol are undecided and out of scope.

## Platform Baseline

Initial development targets the CachyOS stable package available when this
decision was recorded:

- CachyOS/Arch `hyprland 0.56.2-3`, upstream tag `v0.56.2`.
- `hyprgraphics 0.5.1-4`, ABI package `libhyprgraphics.so.4`.
- C++23 and Hyprland's plugin headers.

Hyprland passes C++ objects across the plugin interface and provides no stable
ABI. Build the plugin against the target machine's installed Hyprland headers,
preferably through `hyprpm`. Refuse an incompatible plugin API/hash instead of
attempting a best-effort load. The actively supported target is the current
CachyOS stable Hyprland version; do not maintain parallel implementations for
old versions unless that policy is explicitly changed.

Hyprland itself has no wallpaper module to replace. Official `hyprpaper` is a
separate layer-shell client. Luxaxis adds wallpaper rendering inside the
compositor, reusing Hyprgraphics image decoding and Hyprland texture/render
primitives where appropriate. It does not embed Hyprtoolkit or hyprpaper's
Wayland client loop.

Primary-source references:

- Hyprland plugin interface warning and version API:
  <https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/plugins/PluginAPI.hpp>
- Hyprland texture interface:
  <https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/Texture.hpp>
- Hyprgraphics image and async resource interfaces:
  <https://github.com/hyprwm/hyprgraphics/tree/main/include/hyprgraphics>
- hyprpaper's independent Hyprtoolkit layer-shell implementation:
  <https://github.com/hyprwm/hyprpaper/blob/main/src/ui/UI.cpp>

Before adapting integration code to a newer CachyOS Hyprland release, compare
these interfaces and update this baseline in the same commit as the adapter.

## Ownership And Render Order

Luxaxis replaces Noctalia wallpaper ownership on managed outputs. Users should
disable Noctalia wallpaper and other wallpaper daemons on those outputs.
Luxaxis never stops third-party processes or edits their configuration.

The required compositor order is:

```text
Hyprland clear color
background layer-shell surfaces
Luxaxis opaque wallpaper pass
bottom layer-shell surfaces (including Noctalia desktop widgets)
normal windows
top and overlay surfaces
```

This order makes standard background-layer wallpaper clients visually harmless
if one is accidentally left running, although they still waste resources.
Clients drawing wallpapers in bottom, top, or overlay layers are unsupported
and must be disabled. Luxaxis must not suppress arbitrary layer-shell surfaces,
because it cannot reliably distinguish a wallpaper from a legitimate desktop
surface.

An excluded output receives no Luxaxis wallpaper pass. Another wallpaper
client may own it. Clean plugin unload leaves the Hyprland clear color or any
still-running background client visible; no fallback daemon is bundled.

## Workspace And Output Semantics

- Only positive numeric normal workspace IDs are modeled.
- Special, named, zero, and negative workspaces do not change Luxaxis state.
- Opening a special workspace leaves the underlying normal workspace profile
  active.
- A mapping may be configured before its workspace exists at runtime. Luxaxis
  never creates, preserves, renames, or otherwise configures workspaces.
- The mapping belongs to the workspace. Moving a workspace to another output
  moves its profile with it.
- Each output independently resolves its visible normal workspace.
- Switching one output never changes another output's wallpaper profile.
- An unmapped workspace resolves to the configured default profile.
- All outputs are managed by default. `excluded_outputs` opts outputs out.

## Wallpaper Behavior

Phase 2A accepts local static PNG, JPEG, and WebP files. Paths are absolute or
start with `~`; network URLs, animated images, and video are out of scope.

Each profile selects one fit mode:

- `cover` (default)
- `contain`
- `stretch`

The normalized `position = [x, y]` controls the crop focal point and defaults to
`[0.5, 0.5]`.

Image decoding and file IO never run on the render thread. If a newly activated
image is not cached, retain the old wallpaper until the new texture is ready.
If a watched file temporarily disappears, retain its last valid texture. A
successfully decoded replacement swaps atomically.

Use a global byte-accounted LRU texture cache with a default `256 MiB` budget.
Active and in-transition textures are pinned and may temporarily exceed the
budget; evict back to budget when they become unpinned. Release CPU decoding
buffers after GPU upload.

## Spotlight Model

Spotlight affects only the Luxaxis wallpaper pass. It never dims windows, bar
widgets, desktop widgets, or the cursor.

Every non-`none` Spotlight has a mask color and opacity. The reveal region
reduces mask coverage to expose the original wallpaper; it does not brighten
the wallpaper above its original value.

Spatial lengths accept either logical pixels (`"240px"`) or a percentage of
the output's shorter logical edge (`"18%"`). Coordinates are normalized output
coordinates unless explicitly documented otherwise.

Supported Phase 2A types:

### `none`

Draw the original wallpaper without a mask. This remains true on active and
non-active outputs.

### `circle`

A cursor-centered circular reveal with configurable `radius` and `softness`.

### `strip`

A cursor-centered strip spanning the active output. It supports `horizontal`
and `vertical` orientations plus configurable `thickness` and `softness`.

### `fan`

The configured normalized `anchor` is the light source. The cursor is the
center of an ellipse and not merely a direction control. Let `A` be the anchor,
`C` the cursor, `d` the normalized `A -> C` axis, and `n` its perpendicular.
The ellipse's transverse endpoints are `C +/- n * radius`. The reveal region is
the union of that ellipse and triangle `(A, P1, P2)`. The longitudinal ellipse
radius is `radius * aspect_ratio`; the ellipse rotates with `d`.

The fan does not continue beyond the cursor-centered ellipse. Near a degenerate
`A == C`, preserve the last valid direction and smoothly collapse the triangle
toward the ellipse. The beam may increase reveal strength from anchor to
ellipse; a start-reveal value of `1` produces a uniform beam. Shape boundaries
use configurable softness.

## Active Output

Exactly one managed output may show a Spotlight reveal region:

- `active_output = "cursor"` (default) selects the output containing the
  cursor.
- `active_output = "focused"` selects Hyprland's focused output.

Every other managed output keeps the mask defined by its current profile but
has no reveal region. A `none` profile never gains a mask. In focused mode the
global cursor geometry is clipped normally; when the cursor lies outside the
focused output, the reveal may be partially or completely outside it.

Active-output changes transfer the reveal immediately in Phase 2A. Geometry
does not span outputs. Spotlight follows compositor frames while the cursor is
moving and adds at most one compositor frame of latency.

The temporary `spotlight on`, `off`, and `toggle` overrides affect only the
current compositor session. `off` treats every profile as `none` without
changing wallpaper or writing configuration. Plugin reload clears the override.

## Transition Model (Phase 2B)

Transition is runtime-optional and owned by the destination profile. Missing
configuration falls back to the global transition; `type = "none"` switches
immediately.

The accepted types are:

- `fade`: cross-fade old and new wallpaper.
- `wipe`: reveal behind a moving straight edge.
- `grow`: reveal inside an expanding circle from the origin.
- `outer`: reveal from output edges inward toward the origin.
- `clock`: angular sweep around the origin.
- `random`: choose from a configured non-random allowlist without an immediate
  repeat.
- `none`: no animation.

Origins are `"cursor"`, `"center"`, or normalized `[x, y]`. Default duration
is `220ms`, with configurable easing and an accepted range of `50..2000ms`.

On a second workspace change during an animation, do not queue. Capture the
currently composited wallpaper result and transition from it to the newest
destination. The cursor remains live throughout.

Spotlight changes share the wallpaper transition interval. Interpolate
parameters for equal Spotlight types; cross-fade evaluated masks for different
types, treating `none` as a transparent mask. When two profiles use the same
wallpaper texture, skip texture blending and transition only the Spotlight.

Workspace activation and workspace moves trigger configured transitions.
Configuration reload and successful file replacement use a short fade. Initial
plugin load and output hotplug show their result immediately.

## Configuration Contract

The plugin reads `$XDG_CONFIG_HOME/hypr/luxaxis.toml`, falling back to
`~/.config/hypr/luxaxis.toml` when `XDG_CONFIG_HOME` is unset. It never writes
or reformats the file. Watch it with debounce and provide a manual `reload`
dispatcher for diagnostics.

The parser is strict. Unknown fields, duplicate workspace IDs, missing profile
references, invalid colors, invalid units, and out-of-range values reject the
entire candidate configuration. Continue using the last valid configuration
and report the TOML path plus a concrete error. A single image load failure
falls back to the default profile; if that also fails, draw the configured
opaque fallback color and report the failure once.

Canonical Phase 2A shape:

```toml
version = 1
default_profile = "default"
active_output = "cursor"
excluded_outputs = []
texture_cache_mib = 256
fallback_color = "#000000"

[profiles.default]
wallpaper = "/absolute/path/default.webp"
fit = "cover"
position = [0.5, 0.5]

[profiles.default.spotlight]
type = "none"

[profiles.focus]
wallpaper = "~/Pictures/focus.png"
fit = "cover"
position = [0.5, 0.5]

[profiles.focus.spotlight]
type = "circle"
mask_color = "#000000"
mask_opacity = 0.55
radius = "18%"
softness = "4%"

[profiles.beam]
wallpaper = "~/Pictures/beam.jpg"

[profiles.beam.spotlight]
type = "fan"
mask_color = "#102b12"
mask_opacity = 0.68
anchor = [0.5, 0.08]
radius = "18%"
aspect_ratio = 0.55
softness = "4%"
beam_start_reveal = 0.35

[workspaces]
"1" = "default"
"2" = "focus"
"3" = "beam"
```

Phase 2B extends profiles with an optional transition table without changing
the Phase 2A keys.

## Module Design

Keep Hyprland internals behind one adapter seam. Domain modules must compile and
test without Hyprland headers.

```text
Config loader ----> Engine <---- Hyprland event adapter
                       |
                       v
                  Render plan
                       |
          Image cache + Hyprland renderer adapter
```

The Engine is the deep module. Its interface accepts validated configuration,
output/workspace/cursor events, and time; it returns immutable per-output render
plans. It owns profile resolution, active-output policy, fallback behavior,
Spotlight parameters, and transition state. It performs no IO and makes no GL
calls.

The config loader owns TOML, path expansion, strict validation, and candidate
versus active configuration. The image cache owns asynchronous decoding,
upload requests, pinning, and byte-accounted LRU eviction. The renderer adapter
owns target-version Hyprland headers, render-stage insertion, texture upload,
shader programs, output transforms, and damage. Do not expose Hyprland objects
through the Engine interface.

Use Hyprgraphics for asynchronous image decoding. Use Hyprland texture and
render-pass primitives at the adapter seam. Do not run a nested Hyprtoolkit
backend or embed hyprpaper's Wayland client.

## Render And Damage Requirements

- Insert an opaque wallpaper pass after background layer-shell rendering and
  before bottom layer-shell rendering.
- Correctly handle fractional scale, output transform/rotation, mirrored
  outputs, and hotplug.
- Use the compositor's standard sRGB texture path. HDR, ICC, and wide-gamut
  fidelity are not Phase 2 promises.
- Decode and perform filesystem IO off the render thread.
- Do not continuously redraw while idle.
- While Spotlight moves, damage the union of old and new effect bounds and
  present at the active output's compositor cadence.
- During a transition, render continuously only until completion.
- Stop Spotlight damage when an opaque fullscreen surface fully occludes the
  wallpaper.
- Cached workspace changes and cursor motion must reach rendering within one
  compositor frame.

## Milestones And Commits

Use reviewable commits and preserve the documentation-only commit preceding
implementation.

### Phase 2A core

1. C++ project skeleton and target-version build probe.
2. Domain types, strict TOML configuration, and tests.
3. Engine workspace/output state and render-plan tests.
4. Spotlight CPU reference geometry and shader pixel tests.
5. Async image cache and LRU tests.
6. Hyprland adapter, wallpaper render stage, damage, and lifecycle.
7. Integration verification and two-axis code review; fix every material
   finding before declaring Phase 2A complete.

Phase 2A is complete when workspace wallpaper switching, `none/circle/strip/fan`,
multi-output behavior, hot reload, async loading, and failure recovery pass the
test matrix below. It is independently releasable.

### Phase 2B transitions

Implement the accepted transition state machine and shaders after Phase 2A is
reviewed and fixed. Add interruption and same-wallpaper tests, then repeat the
two-axis review. Phase 2B is an enhancement and does not retroactively block
Phase 2A completion.

## Verification Matrix

Automated tests must cover:

- Strict parsing, path expansion, and atomic config replacement.
- Workspace/profile/output resolution and default fallback.
- CPU reference geometry for all four Spotlight types, including fan rotation
  and anchor/cursor degeneracy.
- Shader offscreen pixels at centers, boundaries, softness bands, rotations,
  and output transforms.
- Image decode failures, pinned LRU behavior, file replacement, and byte
  accounting.
- Phase 2B transition completion, interruption, random non-repeat, and
  Spotlight synchronization.

Real Hyprland verification before release must cover:

- Rapid single-output workspace switching.
- Two outputs with different resolution and fractional scale.
- Workspace movement between outputs.
- Cursor and focused active-output policies.
- Rotation, mirroring, and hotplug.
- Malformed configuration, deleted/corrupt image, and atomic image replacement.
- Fullscreen occlusion stopping unnecessary damage.
- Plugin reload and clean unload without a Hyprland crash.

Performance acceptance:

- No render-thread image decode or filesystem IO.
- No continuous repaint while idle.
- No memory growth across repeated switches after cache eviction settles.
- Cached wallpaper and cursor changes become visible within one compositor
  frame.
- Record CPU and GPU timing on a 4K development output and reject measurable
  sustained frame-rate regression; do not claim one universal millisecond
  guarantee across all hardware.

## Explicit Non-Goals

- Niri or another compositor.
- Named, negative, or special workspaces.
- Creating or editing Hyprland workspace configuration.
- Network wallpaper download, animated image, or video wallpaper.
- Dimming application windows or shell surfaces.
- A Noctalia settings frontend or stable Noctalia-to-Hyprland control protocol.
- Automatic termination or suppression of arbitrary wallpaper clients.
- Stable binary compatibility across Hyprland versions.
- HDR/ICC/wide-gamut guarantees in Phase 2.
- Selector, greeter, or lock-screen features from the reference video.

## Reference Video

`/mnt/f/reddit-1wik2gv-hyprland-simple-rice.mp4` is a visual reference only.
It shows fan, circular, and strip Spotlight forms and short wallpaper
transitions labelled `FADE`, `WIPE`, `GROW`, `OUTER`, `CLOCK`, and `RANDOM`.
It does not establish workspace persistence, multi-output behavior, or an
implementation mechanism. The decisions in this specification override any
ambiguous interpretation of the video.
