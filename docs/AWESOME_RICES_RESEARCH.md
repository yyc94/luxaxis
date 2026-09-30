# Hyprland Rice Implementation Routes

Checked 2026-09-28 against the [awesome-rices Hyprland index at commit
`04825ade`](https://github.com/zemmsoares/awesome-rices/blob/04825ade79944a6022b6f84a309f4c506d56f4c9/README.md#hyprland)
and the linked projects' first-party READMEs/source trees. The index is a
showcase of links and screenshots; it does not describe each rice's software
stack. The examples below are a sample, not a census or a ranking by frequency.

| Route beyond Hyprland configuration | Verified examples | What is implemented |
| --- | --- | --- |
| Compose existing standalone tools | [HyDE's source tree](https://github.com/HyDE-Project/HyDE/tree/master/Configs/.config) has `waybar`, `rofi`, `swaync`, `dunst`, and `wlogout` directories. The [older hyprdots README](https://github.com/prasanthrangan/hyprdots/blob/main/README.md) documents Waybar and Rofi actions; [debuggyo/dots](https://github.com/debuggyo/dots/blob/main/README.md) lists Waybar, Wofi, and hyprpaper. | Style/configure a bar and launcher, combine separate notification, wallpaper, lock/logout tools, and coordinate them with scripts and theme generation. The older hyprdots repository declares itself unmaintained and points to HyDE. |
| Build a custom graphical shell with Quickshell/QtQuick/QML | [ArchEclipse](https://github.com/AymanLyesri/ArchEclipse/blob/master/README.md) explicitly documents its own `shell.qml`, bar, launcher, side panels, and theming; its [`.config/quickshell`](https://github.com/AymanLyesri/ArchEclipse/tree/master/.config/quickshell) contains the implementation. [end-4/dots-hyprland](https://github.com/end-4/dots-hyprland/blob/main/.github/README.md) calls its current UI mostly a custom graphical shell and names Quickshell for its bar and sidebars; see [source](https://github.com/end-4/dots-hyprland/tree/main/dots/.config/quickshell). | Program the bar, launcher, widgets, panels, and reactive state as one shell application. Hyprland still handles composition/window management. |
| Build a custom widget shell with AGS or Eww | [end-4's README](https://github.com/end-4/dots-hyprland/blob/main/.github/README.md#previous-styles) records earlier AGS and Eww versions as unsupported, with source in `ii-ags` and `archive` branches. [ArchEclipse's README](https://github.com/AymanLyesri/ArchEclipse/blob/master/README.md#widgets-quickshell--qml) says it migrated from Eww/AGS to Quickshell on 2026-09-12. | Write custom bar/panel widgets on another framework; the cited instances are historical versions, not the current stack of those projects. |

Noctalia is another *possible* host-shell route, but this sample does not verify
an awesome-rices entry using it. The collection README itself does not identify
stacks or mention Noctalia. Luxaxis demonstrates the route locally: its
[README](../README.md) documents an independently usable Noctalia plugin for a
workspace bar and style panel. On the repository's verified Noctalia v5.1.0
baseline, the host is native C++/GLES with Luau plugins, **not** the old v4
Quickshell/QML shell ([verification](VALIDATION.md),
[upstream plugin API](https://github.com/noctalia-dev/noctalia/blob/v5.1.0/docs/plugin-api.json)).
This is a plugin/extension of an existing shell, distinct from writing an entire
Quickshell shell. Wallpaper effects that must draw inside the compositor are a
separate concern; Luxaxis uses an independent Hyprland plugin for them
([accepted specification](HYPRLAND_PLUGIN.md)).

The linked project READMEs describe their current default branches as checked
on the date above; those projects can change. Screenshots alone do not establish
which UI toolkit, shell, or wallpaper renderer produced them.
