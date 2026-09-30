# Open-source alternatives to Noctalia

Checked 2026-09-28 against the projects' first-party READMEs and GitHub
repository metadata. Here "same niche" means an installable Wayland desktop
shell that provides several everyday desktop surfaces (bar, launcher, controls,
notifications, etc.), not merely a widget toolkit or a Hyprland theme. Feature
lists below reflect project claims; no local installation was tested.

| Project | Stack and scope | Hyprland fit / distinguishing point |
| --- | --- | --- |
| [DankMaterialShell](https://github.com/AvengeMedia/DankMaterialShell/blob/master/README.md) | Quickshell/QML frontend plus Go backend/CLI; bar, launcher, control center, notifications, wallpaper/theming, session management, and a [plugin registry](https://github.com/AvengeMedia/DankMaterialShell/blob/master/README.md#plugin-system). | Explicitly supports Hyprland and several other Wayland compositors; its README calls it a complete desktop shell. Closest general-purpose alternative for someone seeking a configurable, packaged shell with extensions. |
| [Wayle](https://github.com/wayle-rs/wayle/blob/master/README.md) | Rust, GTK4 and Relm4; bar, notifications, OSD, wallpaper, device controls, settings GUI, CLI, TOML configuration and custom bar modules. | Targets Wayland layer-shell compositors; README names Hyprland, Niri and Mango for compositor-specific modules. It is the [successor named by HyprPanel](https://github.com/Jas-SinghFSU/HyprPanel/blob/master/README.md). More GTK/Rust-oriented than the Quickshell choices. |
| [Caelestia Shell](https://github.com/caelestia-dots/shell/blob/main/README.md) | Quickshell/QML shell with bar, launcher, dashboard, notifications, lock/session UI and configurable wallpaper/theme integration; separate `caelestia` CLI. Packaged separately from its [full dotfiles](https://github.com/caelestia-dots/caelestia). | Explicitly built for Hyprland. Its README documents AUR/Nix/manual installs, JSON settings, per-monitor overrides and source editing for deeper customization. |
| [Ambxst](https://github.com/Axenide/Ambxst/blob/main/README.md) | Quickshell/QML frontend with a Go backend ([source layout](https://github.com/Axenide/Ambxst)); launcher, controls, notifications, wallpaper manager, workspace management, settings and a [mod system](https://github.com/Axenide/Ambxst/blob/main/docs/mods/README.md). | Supports Hyprland and Niri. The install path imports generated compositor configuration; this is a more opinionated desktop setup. It is [Ax-Shell's stated successor](https://github.com/Axenide/Ax-Shell/blob/main/README.md). |
| [end-4 / illogical-impulse](https://github.com/end-4/dots-hyprland/blob/main/.github/README.md) | Quickshell/QML status bar and sidebars inside a larger Hyprland dotfiles project, with launcher/settings/overview and other integrated tools. | A complete rice and a rich source example, but the README presents it as Hyprland dotfiles, not a compositor-independent shell package. It currently uses Quickshell; earlier AGS/Eww styles are explicitly unsupported. |

The first three above have especially clear standalone shell installation paths.
Ambxst is also installable as a shell but deliberately manages part of the
compositor configuration. end-4 is useful when adopting its whole Hyprland
desktop or studying the implementation, rather than swapping only a shell.
GitHub metadata showed all five repositories unarchived on the check date
([DMS](https://api.github.com/repos/AvengeMedia/DankMaterialShell),
[Wayle](https://api.github.com/repos/wayle-rs/wayle),
[Caelestia](https://api.github.com/repos/caelestia-dots/shell),
[Ambxst](https://api.github.com/repos/Axenide/Ambxst),
[end-4](https://api.github.com/repos/end-4/dots-hyprland)).

**Historical choices:** [HyprPanel](https://github.com/Jas-SinghFSU/HyprPanel/blob/master/README.md)
is an AGS/Astal Hyprland panel whose README says development moved to Wayle;
[Ax-Shell](https://github.com/Axenide/Ax-Shell/blob/main/README.md) is a
Fabric/Python Hyprland shell whose README says it was superseded by Ambxst.
Both repositories are [archived](https://api.github.com/repos/Jas-SinghFSU/HyprPanel)
([Ax-Shell metadata](https://api.github.com/repos/Axenide/Ax-Shell)); they are
references, not first recommendations for a new installation.

Quickshell, AGS/Astal, Eww and Fabric are **frameworks/toolkits** used to build
shells, not themselves Noctalia-equivalent finished desktops. The projects
above illustrate the distinction: a Quickshell-based shell can be an end-user
product (DMS, Caelestia, Ambxst) or part of a particular dotfiles setup
(end-4). On this repo's [Noctalia v5.1.0 baseline](VALIDATION.md), Noctalia
itself is a native C++/GLES shell with Luau plugins, not its older Quickshell
implementation.
