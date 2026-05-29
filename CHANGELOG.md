# Changelog

All notable changes to snapx are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.3.0] - 2026-05-29

### Added

- **Upload / share** — Imgur or custom POST URL via optional libcurl; Settings UI, CLI flags, async upload with cancel
- **OCR** — Copy text from captures via optional Tesseract; async processing with Stop, status strip and Settings hints
- **Recent saves** — File-list overlay toggled from **Recents** button in the action bar
- **Annotation tools** — Numbered callouts, solid redaction, text popover entry, post-capture crop, pin-to-screen
- **Capture overlay** — Magnifier loupe (Space), window snap highlight, window list for region mode
- **Background mode** — System tray, close-to-tray, `--background`, autostart desktop entry
- **Session D-Bus** — `CaptureRegion`, `Show`, `Save`, `GetLastPath` on `io.github.snapx`
- **Flatpak** manifest and CI bundle job

### Fixed

- **Blur / pixelate** tool now mutates base image pixels (was a no-op overlay stroke)
- Upload and OCR buttons always visible with capability status; disabled states and Settings shortcuts when unavailable
- OCR and upload run off the main thread; in-flight jobs can be cancelled

### Changed

- Workflow status strip above action bar (Upload / OCR readiness)
- Recent-captures sidebar redesigned: compact filename list, no permanent canvas gutter

[1.3.0]: https://github.com/Prem01-cyber/snapx/releases/tag/v1.3.0

## [1.2.1] - 2026-05-29

### Fixed

- **Color picker crash** in Settings and toolbar (`gtk_color_dialog_choose_rgba` assertion after premature unref)
- **Settings dialog switches** (Include cursor, Auto copy, Play sound) stretching to full row width
- **Shortcuts tab** layout: grid with Action / Shortcut / Record / Clear columns
- GTK markup warning for section titles containing `&` (e.g. Global & capture)
- Debug build link failure when `libasan` is not installed (AddressSanitizer now opt-in)
- Linux CI on older PipeWire (`pw_stream_update_params` fallback) and ubuntu-24.04 runner

### Changed

- Settings dialog: separate Options groups for toggles, improved pref-row alignment
- README badges: static release and per-platform badges (shields.io GitHub API was unreliable)

[1.2.1]: https://github.com/Prem01-cyber/snapx/releases/tag/v1.2.1

## [1.2.0] - 2026-05-29

### Added

- **Linux Wayland global hotkeys** via `org.freedesktop.portal.GlobalShortcuts` (GNOME 45+ / KDE 6+)
- Host portal app-id registration (`io.github.snapx`) for native (non-Flatpak) installs
- `io.github.snapx.desktop` alongside `snapx.desktop` in Linux packages and AppImage

### Fixed

- Portal `Activated` signal parsing (`(osta{sv})`) so global shortcuts fire without GLib CRITICAL errors
- Session handle extraction and D-Bus variant building for `BindShortcuts`
- Global hotkeys on Wayland when snapx is installed (desktop file + binary in `PATH`)

[1.2.0]: https://github.com/Prem01-cyber/snapx/releases/tag/v1.2.0

## [1.1.0] - 2026-05-29

### Added

- Configurable keyboard shortcuts in Settings (capture, editor, region overlay)
- Per-mode capture shortcuts: full screen, monitor, region, window (defaults `Ctrl+Shift+1`–`4`)
- Filename pattern tokens: `%d`, `%03d`, `%n`, date/time tokens with live preview
- Annotation defaults in Settings (default tool and color applied to toolbar)
- Multi-action global hotkeys on X11 and Windows
- Fedora/RHEL RPM build script ([`packaging/linux/build-rpm.sh`](packaging/linux/build-rpm.sh))

### Changed

- Settings dialog UI: scrolled tabs, shortcut sections, Record/Clear/Reset controls
- In-app shortcuts use capture-phase key handler (works when canvas/toolbar focused)
- Header button tooltips reflect configured shortcut keys

### Fixed

- GtkShortcutController trigger use-after-free (`GTK_IS_SHORTCUT_TRIGGER` CRITICAL)
- Wayland multi-monitor region overlay GdkMonitor stability
- Global hotkey callback registration (was never wired)
- Play sound on capture now uses system beep when enabled

[1.1.0]: https://github.com/Prem01-cyber/snapx/releases/tag/v1.1.0

## [1.0.0] - 2026-05-29

### Added

- Cross-platform screenshot utility for Linux, Windows, and macOS
- Capture modes: full screen, single monitor, region, active window, delayed capture
- GTK4 GUI (GTK3 fallback) with Cairo annotation canvas
- Annotation tools: rectangle, arrow, pen, text, blur, highlight with undo/redo
- Output formats: PNG, JPEG (quality 1–100), WebP
- Clipboard copy (manual and auto-copy setting)
- Global hotkey and in-app keyboard shortcuts
- Headless CLI (`--no-gui`) for scripting and automation
- Persistent INI configuration with settings dialog
- **Linux Wayland**: XDG Screenshot portal (default, GNOME/Fedora-style)
- **Linux Wayland**: optional ScreenCast + PipeWire one-frame fallback
- **Linux Wayland**: per-monitor 1:1 region overlay on multi-monitor setups
- **Linux X11**: libX11 + XRandR + XFixes capture
- **Windows**: DXGI Desktop Duplication (10+) with GDI fallback (7/8)
- **macOS**: ScreenCaptureKit (12.3+) with CoreGraphics fallback

### Fixed

- GdkMonitor use-after-free in per-monitor region overlay (stability on multi-monitor Wayland)
- GTK4 clipboard lifetime during async store
- PipeWire stream teardown ordering when ScreenCast fallback is used

[1.0.0]: https://github.com/Prem01-cyber/snapx/releases/tag/v1.0.0
