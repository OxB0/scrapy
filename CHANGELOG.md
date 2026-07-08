# Changelog

All notable changes to **scrapy** (a scrcpy fork with a built-in toolbar,
terminal, and drawers) are documented here. Based on scrcpy 4.0.

## [1.2] — 2026-07-08

### Added — Terminal
- **Text selection** — click-and-drag to select terminal text, with a live highlight.
- **Copy** — right-click a selection, or press `Ctrl+C`, to copy it. `Ctrl+Shift+C` always copies. With no selection, `Ctrl+C` still sends SIGINT.
- **Paste** — right-click with no selection, or `Ctrl+V` / `Ctrl+Shift+V`. Large pastes (over 30 lines or 3000 characters) ask for confirmation first.
- **Drag & drop** — drag text from another app and drop it onto the terminal to paste it.
- **Layout-independent typing** — the terminal always sends US-QWERTY English regardless of the active OS keyboard layout (Hebrew, Russian, Arabic, …), and every `Ctrl`+letter shortcut works in any layout.

### Added — Notifications
- **On-screen banner** at the bottom of the window after actions: installing a file, copying a file, screenshots, and recordings. Green on success, red with the failure reason on error; long messages wrap across lines.
- The install/copy failure reason is captured from adb and shown in the banner.
- Configurable in `scrapy.conf`: `notifications` (on/off), `notification_time` (seconds), `notification_text_size`.
- Suppressed automatically while the log drawer is open (the log already shows the result).

### Added — Log drawer
- **New "Log" toolbar button** (and `Alt+Shift+L`) opens a side panel showing the live scrcpy log — including FFmpeg — so it is visible even without a console. Warnings are amber, errors red; scroll with the wheel, PageUp/PageDown, Home/End.
- Removable like any other button via the `buttons` line; width set with `log_width`, shortcut with `key_log`.

### Changed
- **Redesigned toolbar icons** — replaced the hand-drawn shapes with the Lucide
  icon set (rasterized with nanosvg, anti-aliased and tinted per state) for a
  cleaner, modern look.
- The device picker window is wider so the "Waiting for a device…" hints are no longer clipped.
- Toast/notification text is rendered in batched draw calls, so a visible message no longer stutters the mirror.
- Screenshot/recording/keep-awake feedback now uses the same unified notification banner.

## [1.1] — 2026-07-07

### Fixed
- On device disconnect, the process now re-launches itself for a clean adb
  state, so reconnecting a device is reliably detected instead of hanging on
  "waiting for device".

## [1.0] — 2026-07-07

### Added
- Initial **scrapy** fork of scrcpy 4.0; the release binary is named `scrapy`.
- **Startup device picker**: opens immediately, waits when nothing is connected,
  auto-connects a lone device, and lets you choose when several are present.
- **On-screen toolbar** over the mirror: pin-on-top, keep-awake, shell, apps,
  screenshot, record, and the device keys (back, home, recents, menu,
  notifications, volume, rotate, power).
- **Built-in terminal drawer** (`adb shell`) with scrollback and word wrap.
- **Apps & density drawer**: launch installed apps and change display density.
- **Screenshot and screen-record** buttons that save to the home folder.
- **Configuration file** (`scrapy.conf`): choose/reorder toolbar buttons,
  drawer widths, capture folder, default density, terminal text size, and fully
  configurable keyboard shortcuts.
- **Native Linux build** alongside the Windows build.
