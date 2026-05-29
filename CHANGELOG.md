# Changelog

All notable changes to snapx are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
