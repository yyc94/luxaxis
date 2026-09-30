# Using cbirds with Fastfetch

Checked 2026-09-28 against [cbirds at `cc446fc3`](https://github.com/clainstone/cbirds/blob/cc446fc3cb80733371c62676533adcac2fc10002/README.md) and Fastfetch's [official logo options](https://github.com/fastfetch-cli/fastfetch/wiki/Logo-options).

**Yes, as a recorded image logo.** cbirds can render a flock headlessly to an animated GIF with `--record`; `--record-size`, `--record-seconds`, and `--record-fps` control the recording. Fastfetch can use that GIF as a `kitty` image logo. Its default `--logo-animation-frame 1` displays only the first frame; `0` plays the animation. On Linux/BSD, Fastfetch's `kitty` image logo requires a build with ImageMagick 6 or 7, and the terminal must support the Kitty graphics protocol. Check the local build with `fastfetch --list-features` ([cbirds recording](https://github.com/clainstone/cbirds/blob/cc446fc3cb80733371c62676533adcac2fc10002/README.md#recording), [Fastfetch animation and kitty requirements](https://github.com/fastfetch-cli/fastfetch/wiki/Logo-options#animation)).

```sh
cbirds --record birds.gif --record-size 60x18 --record-seconds 4 --record-fps 25
fastfetch --kitty ./birds.gif --logo-animation-frame 0 --logo-width 30
```

This uses a pre-recorded clip, so the birds do not respond to the pointer or cbirds' keyboard controls inside Fastfetch. cbirds' normal mode continuously draws to the terminal and accepts input; Fastfetch's `command-raw` logo runs a command and displays its output, so it is not an equivalent live cbirds integration ([cbirds use and controls](https://github.com/clainstone/cbirds/blob/cc446fc3cb80733371c62676533adcac2fc10002/README.md#use), [Fastfetch `command-raw`](https://github.com/fastfetch-cli/fastfetch/wiki/Logo-options#command-raw)).

For a still logo, omit `--logo-animation-frame 0` to show the GIF's first frame. cbirds also supports `--snapshot frame.png --frames 400`, but its README says snapshots require a terminal; GIF recording is the simpler unattended route ([cbirds recording](https://github.com/clainstone/cbirds/blob/cc446fc3cb80733371c62676533adcac2fc10002/README.md#recording), [Fastfetch animation](https://github.com/fastfetch-cli/fastfetch/wiki/Logo-options#animation)).

No local runtime test was performed: neither `fastfetch` nor `cbirds` is installed in this workspace environment. The command sequence follows the two projects' documented options.
