# Changelog

## Unreleased

- Add a `hyprpm.toml` manifest for the Hyprland plugin. `hyprpm` builds only
  `luxaxis.so` using its matching Hyprland headers; Noctalia remains a separate
  installation. The manual CMake installation path remains available.
- Validate the manifest with toml++ and verify that its build commands produce
  the declared plugin file in an isolated checkout. Live `hyprpm add/enable`
  remains unverified until tested on a Hyprland desktop.

## 2026-09-28 (`1ab433a`)

The Noctalia and Hyprland plugins remain separate installable products. Their
installation methods have not changed, but existing installations must replace
their old artifacts to receive these fixes.

### Noctalia plugin

- Target the released Noctalia v5.1.0 plugin API: declare API 28 instead of the
  unsupported API 32, and avoid unreleased color and tooltip APIs.
- Use a persistent floating styles panel to avoid the reproduced close/disable
  crash. Provide inline choices where native menus are unavailable.
- Upgrade by copying the complete manifest and Luau files again, then run
  `noctalia msg config-reload`. Updating only plugin settings is insufficient.

### Hyprland plugin

- Fix the immediate load crash caused by private copies of Hyprland's inline
  compositor globals.
- Render wallpapers, Spotlight masks, and wallpaper transitions with GLES 3.
  Manual workspace changes interrupt the current transition immediately; they
  are never queued. Cursor-driven Spotlight remains live during interruption.
- Set the documented circle example to a 60px radius with an 8px soft edge.
  Existing `luxaxis.toml` files are not modified automatically.
- Upgrade by rebuilding against the installed Hyprland 0.56.2 headers and
  replacing `luxaxis.so`. A TOML-only update cannot repair an old binary.
  The build now requires GLES 3 and `glesv2` pkg-config metadata.

### Verification status

- Passed 10 model and 14 runtime Lua tests, 8 Hyprland CTest cases, and the
  native Noctalia v5.1.0 smoke workflow.
- Executed the production GLES shader in software EGL, checked output pixels,
  and saved [Spotlight previews](artifacts/gpu-preview/), including a native
  1920x1080 sample with a 60px radius.
- Real Hyprland compositor visuals, hardware GPU timing, and the full
  multi-output matrix are still unverified. See
  [validation details](docs/VALIDATION.md) before treating this as a release.
