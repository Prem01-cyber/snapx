# snapx

**A fast, native screenshot tool for Linux, Windows, and macOS**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Version](https://img.shields.io/github/v/release/Prem01-cyber/snapx?color=green)](https://github.com/Prem01-cyber/snapx/releases/latest)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows%20%7C%20macOS%20(x86_64%2Farm64)-lightgrey)](https://github.com/Prem01-cyber/snapx/releases/latest)

snapx captures your screen, lets you select a region across multiple monitors, annotate the result, and save or copy it — without Electron, without a browser, and without proprietary GNOME-only APIs. It is written in **C** with **GTK4** and **Cairo**, designed to feel at home on Fedora, GNOME, KDE, Windows, and macOS.

**[Download the latest release](https://github.com/Prem01-cyber/snapx/releases/latest)** · [Report an issue](https://github.com/Prem01-cyber/snapx/issues) · [Changelog](CHANGELOG.md)

---

## Downloads

Pre-built binaries are on the [Releases](https://github.com/Prem01-cyber/snapx/releases/latest) page. File names include the version (e.g. `snapx-1.2.0-…`).

| Platform | File (pattern) | Install |
|----------|----------------|---------|
| **Linux** (x86_64) | `snapx-*-x86_64.AppImage` | `chmod +x snapx-*-x86_64.AppImage && ./snapx-*-x86_64.AppImage` |
| **Linux** (portable) | `snapx-*-linux-x86_64.tar.gz` | Extract and run `usr/bin/snapx` |
| **Fedora / RHEL** (x86_64) | `snapx-*-*.x86_64.rpm` | `sudo dnf install ./snapx-*-*.x86_64.rpm` |
| **Windows** (64-bit) | `snapx-*-win64-setup.exe` | Run the installer; launch **snapx** from the Start menu |
| **macOS** (Apple Silicon / Intel) | `snapx-*-macos.dmg` | Open the DMG and drag **snapx** to Applications |

> **Note:** CI attaches all platform builds to each [release tag](https://github.com/Prem01-cyber/snapx/releases). If a platform is missing, check the [Release workflow](https://github.com/Prem01-cyber/snapx/actions/workflows/release.yml) for that tag.

### Linux distribution packages

| Distro | Method |
|--------|--------|
| **Fedora / RHEL** | Download the `.rpm` from [Releases](https://github.com/Prem01-cyber/snapx/releases/latest), or build locally: `./packaging/linux/build-rpm.sh` |
| **Arch Linux** | Build from [`packaging/linux/PKGBUILD`](packaging/linux/PKGBUILD) with `makepkg -si` |
| **Flatpak** | Not yet available |

### System requirements (runtime)

| Platform | Requirement |
|----------|-------------|
| Linux (Wayland) | XDG Desktop Portal (`xdg-desktop-portal` + backend, e.g. `xdg-desktop-portal-gnome` on GNOME) |
| Linux (Wayland, global hotkeys) | Portal **GlobalShortcuts** (GNOME 45+ / KDE Plasma 6+); install snapx so `io.github.snapx.desktop` and `snapx` are on `PATH` |
| Linux (Wayland, optional ScreenCast) | PipeWire |
| Linux (X11) | X11 session with RandR |
| Windows | Windows 10 or newer (DXGI; GDI fallback on older builds) |
| macOS | macOS 10.13+ (ScreenCaptureKit on 12.3+) |

The Linux AppImage bundles GTK4 and image libraries. It still relies on your session’s **portal** for Wayland capture (same as most screenshot tools on modern desktops).

### macOS Gatekeeper

Release builds are **not notarized**. On first launch: right-click the app → **Open**, or allow in **System Settings → Privacy & Security**.

---

## Why snapx?

| | snapx | Typical Electron capture apps |
|---|--------|-------------------------------|
| **Runtime** | Native C + GTK4 | Chromium + Node |
| **Memory** | Lightweight | Heavy |
| **Wayland** | XDG Screenshot portal (same family as GNOME Print Screen) | Often broken or portal-incompatible |
| **Multi-monitor region** | Full-desktop grab + **per-monitor 1:1 freeze overlay** | Often one scaled mini-map |
| **Annotation** | Built-in Cairo canvas | Varies |
| **Automation** | Full headless CLI | Rare |

---

## Features

| Category | Details |
|----------|---------|
| **Capture modes** | Full screen, single monitor, region, active window, delayed capture |
| **Multi-monitor** | Enumerate monitors; region selection spans edges with correct HiDPI slicing |
| **Annotation** | Rectangle, arrow, pen, text, blur/pixelate, highlight |
| **Output** | PNG, JPEG (quality 1–100), WebP |
| **Clipboard** | Manual copy or auto-copy after each capture (Settings) |
| **Hotkeys** | Global hotkeys: X11 & Windows (native); **Wayland** via portal GlobalShortcuts; in-app shortcuts always available |
| **GUI** | GTK4 (GTK3 fallback when GTK4 is unavailable) |
| **CLI** | Headless mode for scripts (`--no-gui`) |
| **Settings** | INI config with live filename preview |

---

## Compatibility

| OS | Session / API | Capture backend | GUI |
|----|---------------|-----------------|-----|
| **Linux** | Wayland + portal | XDG Screenshot (default); optional ScreenCast + PipeWire | GTK4 / GTK3 |
| **Linux** | X11 / Xorg | libX11 + XRandR + XFixes | GTK4 / GTK3 |
| **Windows 10/11** | — | DXGI Desktop Duplication | GTK4 (MSYS2) |
| **Windows 8** | — | GDI BitBlt | GTK4 (MSYS2) |
| **macOS 12.3+** | — | ScreenCaptureKit | GTK4 (Homebrew) |
| **macOS 10.13–12.2** | — | CoreGraphics | GTK3 / GTK4 |

**Desktop environments tested or intended:** GNOME (Fedora Workstation), KDE Plasma, Xfce (X11), Windows 10/11, macOS.

**Wayland note:** Third-party apps cannot call GNOME Shell’s private screenshot APIs. snapx uses the same **XDG Desktop Portal** stack as other compliant tools. On Fedora/GNOME this matches built-in screenshot behavior when `wayland_capture_prefer = screenshot` (default).

---

## Quick start

### GUI

```bash
snapx
```

1. Click **Region**, **Full screen**, **Monitor**, or **Window** in the header bar.
2. For region mode: drag on the frozen overlay, press **Enter** to confirm or **Escape** to cancel.
3. Annotate, then **Save** (`Ctrl+S`) or **Copy** (`Ctrl+C`).

Default global hotkey: **`Super+Shift+S`** (Settings → Shortcuts, or `~/.config/snapx/config.ini`). On Wayland, approve global shortcuts in the portal dialog the first time you run snapx.

### CLI (headless)

```bash
# Full screen → file
snapx -m fullscreen -n -o ~/Pictures/screenshot.png

# Region (opens overlay)
snapx -m region

# Active window → clipboard + file
snapx -m active -c -o ~/Pictures/window.png

# Monitor 1, JPEG quality 85
snapx -m monitor -M 1 -f jpeg -q 85 -n -o ~/Pictures/monitor1.jpg
```

### CLI options

| Option | Description |
|--------|-------------|
| `-m, --mode` | `fullscreen` \| `monitor` \| `region` \| `window` \| `active` |
| `-d, --delay` | Pre-capture delay in seconds (0–60) |
| `-M, --monitor` | Monitor index (0-based), `-1` = all |
| `-o, --output` | Output file path (enables headless when set) |
| `-f, --format` | `png` \| `jpeg` \| `webp` |
| `-q, --quality` | JPEG quality 1–100 (default 90) |
| `-c, --clipboard` | Also copy to clipboard |
| `-n, --no-gui` | Capture without showing the main window |
| `-v, --version` | Show version |
| `-h, --help` | Show help |

---

## Wayland (Fedora / GNOME)

| Path | Like built-in Print Screen? | Default? |
|------|----------------------------|----------|
| **Screenshot portal** | Yes | **Yes** |
| **ScreenCast + PipeWire** | No (one frame) | Fallback only |

Set in `~/.config/snapx/config.ini`:

```ini
[capture]
wayland_capture_prefer = screenshot   ; or screencast
```

**Region capture** takes one full-desktop screenshot, then shows a **native 1:1 freeze on each monitor** so selection stays aligned on HiDPI and multi-monitor layouts.

Capture runs on a **background thread** so the UI stays responsive while the portal completes.

### Global hotkeys on Wayland

On Wayland, system-wide shortcuts use **`org.freedesktop.portal.GlobalShortcuts`** (not raw key grabs):

1. Install snapx (RPM, AppImage, or `cmake --install`) so **`snapx` is in `PATH`** and **`io.github.snapx.desktop`** is installed.
2. Launch from the app menu (or an installed `snapx` binary), not a raw `./build/snapx` from the build tree.
3. On first run, approve shortcuts in the **portal dialog** (GNOME Settings → Keyboard also lists them afterward).
4. Defaults: **`Super+Shift+S`** (open snapx), **`Ctrl+Shift+1`–`4`** (capture modes).

On **X11** and **Windows**, global hotkeys use the native grab APIs and do not need the portal.

---

## Configuration

| Platform | Config path |
|----------|-------------|
| Linux | `~/.config/snapx/config.ini` |
| macOS | `~/Library/Application Support/snapx/config.ini` |
| Windows | `%APPDATA%\snapx\config.ini` |

### Example `config.ini`

```ini
[save]
save_dir         = /home/user/Pictures/Screenshots
filename_pattern = screenshot_%Y%m%d_%H%M%S

[capture]
default_mode     = 0
default_delay    = 0
show_cursor      = 0
wayland_capture_prefer = screenshot

[output]
default_format   = 0
jpeg_quality     = 90
auto_clipboard   = 1
play_sound       = 1

[shortcuts]
global_capture   = super+shift+s
capture_fullscreen = ctrl+shift+1
capture_monitor  = ctrl+shift+2
capture_region   = ctrl+shift+3
capture_window   = ctrl+shift+4
save             = ctrl+s
copy             = ctrl+c
undo             = ctrl+z
redo             = ctrl+y
fit              = ctrl+0
zoom_in          = ctrl+plus
zoom_out         = ctrl+minus
region_confirm   = return
region_cancel    = escape
```

### Filename pattern tokens

| Token | Expands to |
|-------|------------|
| `%Y` `%m` `%d` | Year, month, day (in `%Y%m%d`, the `%d` is the day) |
| `%H` `%M` `%S` | Hour, minute, second |
| `%n` | Auto-increment counter (4 digits: `0001`, `0002`, …) |
| `%d` `%i` `%u` | Auto-increment counter (unpadded: `1`, `2`, …) |
| `%03d` `%04d` … | Zero-padded counter (`001`, `0001`, …) |
| `%%` | Literal `%` |

Counters scan your save directory and pick the next free number. Examples: `image_%d` → `image_1.png`, `image_%03d` → `image_001.png`.

All shortcuts can be changed in **Settings → Shortcuts** (Record button captures a key press).

---

## Keyboard shortcuts (defaults)

| Shortcut | Action |
|----------|--------|
| `Super+Shift+S` | Global capture — uses default mode from Capture tab (X11/Windows; in-app on Wayland) |
| `Ctrl+Shift+1` | Capture full screen |
| `Ctrl+Shift+2` | Capture monitor |
| `Ctrl+Shift+3` | Capture region |
| `Ctrl+Shift+4` | Capture active window |
| `Ctrl+S` | Save |
| `Ctrl+C` | Copy to clipboard |
| `Ctrl+Z` / `Ctrl+Y` | Undo / redo annotation |
| `Ctrl+0` | Fit view |
| `Ctrl+Plus` / `Ctrl+Minus` | Zoom in / out |
| `Escape` | Cancel region overlay |
| `Return` | Confirm region selection |

Per-mode capture shortcuts work when snapx is focused. On X11 and Windows they may also be registered globally if the key combo is not already grabbed by another app.

---

## Building from source

### Prerequisites

**Fedora:**
```bash
sudo dnf install cmake gcc gtk4-devel cairo-devel glib2-devel gdk-pixbuf2-devel \
    libX11-devel libXrandr-devel libXfixes-devel libpng-devel libjpeg-turbo-devel \
    libwebp-devel pipewire-devel dbus-devel librsvg2
```

**Ubuntu / Debian:**
```bash
sudo apt install cmake gcc libgtk-4-dev libcairo2-dev libglib2.0-dev \
    libgdk-pixbuf-2.0-dev libx11-dev libxrandr-dev libxfixes-dev \
    libpng-dev libjpeg-turbo8-dev libwebp-dev libpipewire-0.3-dev libdbus-1-dev librsvg2-bin
```

**Arch:** `sudo pacman -S cmake gcc gtk4 cairo glib2 gdk-pixbuf2 libx11 libxrandr libxfixes libpng libjpeg-turbo libwebp pipewire dbus librsvg`

**macOS:** `brew install cmake gtk4 cairo glib gdk-pixbuf libpng libjpeg-turbo webp`

**Windows (MSYS2 UCRT64):** see [packaging/windows/build-installer.sh](packaging/windows/build-installer.sh)

### Build

```bash
git clone https://github.com/Prem01-cyber/snapx.git
cd snapx
bash packaging/icons/generate-icons.sh   # optional if PNGs missing
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/snapx --version
sudo cmake --install build   # optional
```

### Packaging locally

```bash
./packaging/linux/build-appimage.sh 1.2.0      # Linux AppImage + tarball
./packaging/linux/build-rpm.sh 1.2.0           # Fedora/RHEL RPM (on Fedora)
./packaging/macos/build-dmg.sh 1.2.0           # macOS only
# Windows (MSYS2): ./packaging/windows/build-installer.sh 1.2.0
```

---

## Releasing (maintainers)

```bash
git tag -a v1.2.0 -m "snapx 1.2.0"
git push origin v1.2.0
```

GitHub Actions ([`.github/workflows/release.yml`](.github/workflows/release.yml)) builds and uploads the AppImage, Windows installer, and macOS DMG to the GitHub Release.

---

## Troubleshooting

| Symptom | What to do |
|---------|------------|
| `Using Screenshot portal (GNOME/Fedora-style)...` | Normal — default capture path |
| `Removed portal screenshot file: ...` | Portal temp file loaded and deleted |
| `Attempting ScreenCast portal capture...` | Fallback or `wayland_capture_prefer=screencast` |
| Stuck at `capturing frame...` (PipeWire) | Use `wayland_capture_prefer = screenshot` |
| `pw_stream_destroy called from wrong context` | Update snapx; prefer screenshot portal |
| Portal permission denied | Allow screen capture in system settings |
| AppImage won’t run | `chmod +x` and ensure FUSE/`libfuse2` on older distros |
| `VK_SUBOPTIMAL_KHR` Gdk-WARNING | Harmless Vulkan resize warning; safe to ignore |

---

## Project layout

```
snapx/
├── src/           Application source (capture, UI, annotation, output)
├── resources/     Icons, CSS, GResource bundle
├── packaging/     AppImage, Windows installer, macOS DMG, RPM, PKGBUILD
├── .github/       CI and release workflows
└── CHANGELOG.md
```

---

## Contributing

Contributions are welcome. Open an issue or pull request on [GitHub](https://github.com/Prem01-cyber/snapx).

## License

**snapx 1.2.0** is released under the **MIT License** (Copyright © 2026 snapx Team). See [LICENSE](LICENSE) for the full text.

## Acknowledgements

Built with GTK, Cairo, XDG Desktop Portals, PipeWire, and platform-native capture APIs. Inspired by the need for a **small, honest** screenshot tool on modern Wayland desktops.
