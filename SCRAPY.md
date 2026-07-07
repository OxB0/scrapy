scrapy because screen copy is just too long

scrapy is supposed to be a fix for me for scrcpy to all the things that annoy me including:

not working with 2 devices connected \ every disconnect i need to reopen
opening uneeded consoles
needing to open a shell
not being able to easily press home buttons or notifications

and a bunch of more features that I think can come in handy

so in nicer terms:
scrapy is a fork of [scrcpy](https://github.com/Genymobile/scrcpy) that adds an
on-screen control layer to the mirror window, so you can drive the device
without memorising keyboard shortcuts. Everything is drawn inside scrcpy's own
SDL window — there is no separate app and no extra process.

and here are some images for the curious

![Toolbar with every button labelled](doc/scrapy-toolbar.png)


you can also edit the conf file that gets created after the first run to change some of the default settings and looks

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
