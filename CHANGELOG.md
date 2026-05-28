# Changelog

All notable changes to snapx are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
