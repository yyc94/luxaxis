# Eww with CachyOS, Noctalia v5, and Hyprland

Checked 2026-09-28 against first-party documentation/source. **Yes:** Eww can
run as a separate Wayland widget process in the same Hyprland session as
Noctalia. Eww is a standalone widget system, not a replacement compositor or
a Noctalia plugin. The Eww project documents a Wayland build using
`--no-default-features --features=wayland`, with `gtk-layer-shell` as a runtime
dependency; its Cargo features likewise include Wayland and X11 support.
[Eww install docs](https://elkowar.github.io/eww/eww.html),
[Eww Cargo features](https://github.com/elkowar/eww/blob/master/crates/eww/Cargo.toml),
[Eww README](https://github.com/elkowar/eww/blob/master/README.md).

## What needs coordination

- Eww's Wayland windows expose `:stacking` (`fg`, `bg`, `overlay`, `bottom`),
  `:exclusive` (reserve space), `:focusable`, and `:namespace`. Noctalia's bar
  independently exposes position, `reserve_space`, and `layer` (`top` or
  `overlay`). An Eww bar at the same edge as a Noctalia bar can overlap it or
  reserve additional space. Use Eww for a different widget/edge, or disable or
  relocate the corresponding Noctalia bar. A popup may similarly cover an
  existing Noctalia panel or notification by position and layer. These are
  layout conflicts, not an installation or protocol incompatibility.
  [Eww window configuration](https://elkowar.github.io/eww/configuration.html#wayland),
  [Noctalia bar configuration](https://github.com/noctalia-dev/noctalia/blob/main/docs/user/bar/index.mdx).
- Noctalia desktop widgets occupy individual `Bottom` layer-shell surfaces.
  Eww desktop widgets can coexist if their bounds and layer choices avoid
  covering one another.
  [Noctalia desktop widgets](https://github.com/noctalia-dev/noctalia/blob/main/docs/user/desktop/widgets.mdx),
  [Eww window configuration](https://elkowar.github.io/eww/configuration.html#wayland).
- Eww needs its own `eww.yuck` and CSS/SCSS configuration; installing Eww does
  not import Noctalia widgets or create a complete shell. Eww's daemon is
  started with `eww daemon`, then a configured window with `eww open NAME`.
  [Eww configuration](https://elkowar.github.io/eww/configuration.html),
  [Eww running instructions](https://elkowar.github.io/eww/eww.html#running-eww).

## Wallpaper ownership in this repository

With only Noctalia and Eww, Noctalia can retain wallpaper ownership while Eww
adds widgets. If the separate Luxaxis Hyprland plugin manages an output,
disable Noctalia's wallpaper there as the repository's
[deployment guide](../README.md#禁用-noctalia-wallpaper) and
[accepted specification](HYPRLAND_PLUGIN.md#ownership-and-render-order)
require. Eww does not need to be disabled: Luxaxis draws its opaque wallpaper
after `background` layer surfaces and before `bottom` layer surfaces. Thus an
Eww widget on `bottom` or a higher layer can appear over it; a `bg` surface is
covered on managed outputs. An opaque full-screen Eww surface above Luxaxis
would obscure the wallpaper/Spotlight and should not be used as a second
wallpaper owner. This follows the repository's specified render order, not a
separate Eww limitation.

## Installation evidence and limit

The official Eww instructions list Arch dependencies including `gtk3`,
`gtk-layer-shell`, and `libdbusmenu-gtk3`, and give this Wayland build command:

```sh
cargo build --release --no-default-features --features=wayland
```

An Arch [AUR `eww` package](https://aur.archlinux.org/packages/eww) exists and
its [PKGBUILD](https://aur.archlinux.org/cgit/aur.git/plain/PKGBUILD?h=eww)
depends on `gtk-layer-shell`, but the exact installed CachyOS repositories and
packages were not checked on a CachyOS host. No Eww, Noctalia, or Hyprland
runtime test was possible in this environment.
