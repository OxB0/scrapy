# scrapy — scrcpy with a built-in control UI

scrapy is a fork of [scrcpy](https://github.com/Genymobile/scrcpy) that adds an
on-screen control layer to the mirror window, so you can drive the device
without memorising keyboard shortcuts. Everything is drawn inside scrcpy's own
SDL window — there is no separate app and no extra process.

The client is built as a GUI app on Windows, so launching it never opens a
console window.

## Startup device picker & multi-device support

When you launch scrapy without specifying a device, it opens a picker window
immediately and polls adb on a background thread:

- **No device** → shows *“Waiting for a device…”*. If a device is connected but
  adb is holding it *offline* or *unauthorized*, it shows *“Device
  connecting…”* and runs `adb reconnect` to unstick it.
- **One device** → connects automatically, no window shown.
- **Several devices** → lists them so you can pick which one to mirror:

![Device picker with two devices](doc/scrapy-picker.png)

The app then **stays open across reconnects**: if the device is unplugged
(whether it disconnects cleanly or abruptly), scrapy returns to the picker and
reconnects when a device comes back, instead of exiting. Closing the window
quits.

## On-screen toolbar

A vertical toolbar sits in a gutter beside the mirror (the window is widened by
the toolbar width, so the video keeps its natural size). Hovering a button
shows its label; every button labelled at once:

![Toolbar with every button labelled](doc/scrapy-toolbar.png)

| Button | Action |
| --- | --- |
| Pin on top | Keep the window above other windows |
| Keep awake | Stop the device screen from sleeping while plugged in |
| Shell | Open the adb terminal drawer |
| Apps & density | Open the app launcher / display-density drawer |
| Screenshot | Save a PNG of the device to the capture folder |
| Record | Start/stop screen recording (glows red while recording) |
| Back / Home / Recents / Menu | Android navigation keys |
| Notifications | Cycle notifications → quick settings → collapse |
| Volume up / down | Device volume |
| Rotate | Rotate the device |
| Power | Device power button |

## Terminal drawer

The **Shell** button opens a real `adb shell` terminal on the right. It grows to
fill the window width, wraps long lines (the device is told the width via
`stty`), and uses the Terminus font. The video area is untouched when it opens.

## Apps & density drawer

The **Apps & density** button opens a drawer with two tabs:

- **Apps** — a searchable list of installed apps. Left-click launches an app;
  right-click opens it on a **new virtual display** (`--new-display
  --start-app`). A toggle shows system apps too.
- **Density** — the current display density with `-` / `+` / `Reset` controls.

## Config file

On first run, scrapy writes a commented `scrapy.conf` next to the executable.
Edit it and relaunch:

- `buttons` — which toolbar buttons to show, in order (or `none`)
- `shell_width`, `apps_width` — drawer widths
- `capture_dir` — where screenshots and recordings are saved
- `pin_on_top` — start pinned always-on-top
- `default_density` — force a dpi on connect

## Building

Windows client (cross-compiled from Linux/WSL with mingw-w64):

```sh
./release/build_windows.sh 64
```

Based on the great [scrcpy](https://github.com/Genymobile/scrcpy) by Genymobile,
licensed under Apache-2.0.
