# Luxaxis

Luxaxis customizes Hyprland workspaces and their visual identity while keeping
the Noctalia and Hyprland deliverables independently usable.

## Language

**Normal workspace**:
A positive numeric Hyprland workspace. Luxaxis does not model named, negative,
or special workspaces.
_Avoid_: Desktop, virtual desktop

**Workspace profile**:
A named, reusable visual definition containing one wallpaper and optional
Spotlight and transition definitions.
_Avoid_: Workspace theme, wallpaper entry

**Managed output**:
An output on which the Luxaxis Hyprland plugin owns wallpaper rendering.
Excluded outputs remain outside Luxaxis wallpaper ownership.
_Avoid_: Luxaxis monitor, primary monitor

**Active output**:
The single managed output allowed to cut a bright region through its Spotlight
mask. It is selected from the cursor-containing output by default, or from the
Hyprland-focused output when configured.
_Avoid_: Main output, primary output

**Spotlight**:
A workspace-profile effect that tints or darkens a wallpaper while revealing a
cursor-directed region. `none` is a Spotlight type that leaves the wallpaper
unchanged.
_Avoid_: Cursor glow, screen overlay

**Mask**:
The color and opacity applied to a wallpaper outside its Spotlight reveal
region. A non-active output retains this mask without a reveal region.
_Avoid_: Dim layer, window overlay

**Fan Spotlight**:
A Spotlight whose configured anchor connects to the two transverse endpoints
of a cursor-centered ellipse. The triangle and ellipse form one reveal region.
_Avoid_: Infinite cone, cursor-origin beam

**Wallpaper ownership**:
Responsibility for selecting, loading, and drawing the complete opaque
wallpaper of a managed output. Luxaxis replaces Noctalia wallpaper ownership,
not a Hyprland wallpaper module.
_Avoid_: Hyprland wallpaper replacement

**Wallpaper transition**:
A bounded animation from the current composited wallpaper state to a newly
activated workspace profile. It is distinct from Spotlight movement and active
output changes.
_Avoid_: Workspace animation

## Current Repository State

The user-facing deployment guide is available at `README.md`. It documents the
two independent deliverables separately:

- The Noctalia plugin provides the workspace bar widget and style panel.
- The Hyprland plugin provides workspace-bound wallpapers, masks, Spotlights,
  and optional transitions.

The guide includes installation, activation, configuration, multi-output
behavior, temporary controls, conflict warnings for other wallpaper owners,
and removal instructions. It intentionally does not describe internal module
design or development workflow.

The guide was added in commit `a090bf3` (`docs: add user deployment guide`).
The Noctalia wallpaper section was verified against the official Noctalia
commit `58f71922`: users can disable the service in Settings or set
`[wallpaper] enabled = false`, then run `noctalia msg config-reload`.

Luxaxis Spotlight masks use the fixed `mask_color` and `mask_opacity` from
each workspace profile. Luxaxis does not extract wallpaper colors or choose a
mask color automatically. The corresponding deployment-guide update is in
commit `9df46d0` (`docs: document disabling Noctalia wallpaper`).

Future changes should preserve the independent installation and runtime
boundaries between the two plugins.
