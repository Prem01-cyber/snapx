<div align="center">

# snapx

**A fast, native screenshot tool for Linux, Windows, and macOS.**

Capture · annotate · beautify · share — without Electron, without a browser, and without GNOME-only APIs.

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Release](https://img.shields.io/github/v/release/Prem01-cyber/snapx?label=release&color=2ea44f)](https://github.com/Prem01-cyber/snapx/releases/latest)
[![Linux](https://img.shields.io/badge/Linux-x86__64-FCC624?logo=linux&logoColor=black)](https://github.com/Prem01-cyber/snapx/releases/latest)
[![Windows](https://img.shields.io/badge/Windows-x64-0078D4?logo=windows&logoColor=white)](https://github.com/Prem01-cyber/snapx/releases/latest)
[![macOS](https://img.shields.io/badge/macOS-universal-000000?logo=apple&logoColor=white)](https://github.com/Prem01-cyber/snapx/releases/latest)

[**Download**](https://github.com/Prem01-cyber/snapx/releases/latest) · [Quick start](#quick-start) · [Features](#features) · [Build from source](#building-from-source) · [Changelog](CHANGELOG.md)

</div>

---

## Why snapx?

Most screenshot tools are either heavyweight Electron apps or tied to one desktop. snapx is written in **C** with **GTK 4** and **Cairo**: it starts instantly, uses a few megabytes of memory, and captures correctly on **Wayland, X11, Windows, and macOS** using each platform's native APIs. It produces polished, share-ready images — backgrounds, padding, shadows and all — and drives the same engine from a scriptable headless CLI.

| | snapx | Typical Electron capture app |
|---|---|---|
| **Runtime** | Native C + GTK 4 | Chromium + Node |
| **Memory / startup** | Lightweight, instant | Heavy, slow cold start |
| **Wayland** | XDG Desktop Portal (same family as GNOME Print Screen) | Often broken or portal-incompatible |
| **Multi-monitor region** | Full-desktop grab + per-monitor 1:1 freeze overlay | Often one scaled mini-map |
| **Beautiful export** | Built-in (background, padding, shadow, rounded corners) | Plugin or absent |
| **Automation** | Full headless CLI + D-Bus | Rare |

---

## Highlights

- 🖥️ **Every surface** — full screen, a single monitor, a dragged region, or the active window. Multi-monitor and HiDPI aware.
- 🎨 **Annotate** — rectangle, ellipse, line, arrow, freehand pen, text, highlight, numbered callouts, blur/pixelate, redaction, plus a colour **eyedropper**. Undo/redo throughout.
- ✨ **Beautiful export** — wrap any capture in padding, a solid/gradient/transparent background, rounded corners and a soft drop shadow. Live preview, then copy or save. Perfect for docs, slides and social posts.
- 🔤 **Copy text (OCR)** — pull text straight out of a capture (optional Tesseract).
- ☁️ **Share** — upload to Imgur or a custom endpoint and copy the link (optional libcurl).
- 📌 **Workflow** — pin captures on top, post-capture crop, recent-captures sidebar, reveal-in-folder, auto-copy, delayed capture.
- ⌨️ **Hotkeys everywhere** — native global shortcuts on X11/Windows; portal `GlobalShortcuts` on Wayland; in-app shortcuts always available.
- 🤖 **Scriptable** — full headless CLI (`--no-gui`) and a session D-Bus API.
- 🪶 **Native & small** — GTK 4 (GTK 3 fallback), Cairo, platform-native capture. No browser engine.

---

## Install

Pre-built binaries are on the [**Releases**](https://github.com/Prem01-cyber/snapx/releases/latest) page (file names include the version).

| Platform | File pattern | Install |
|----------|--------------|---------|
| **Linux** — AppImage | `snapx-*-x86_64.AppImage` | `chmod +x snapx-*.AppImage && ./snapx-*.AppImage` |
| **Linux** — portable | `snapx-*-linux-x86_64.tar.gz` | Extract, run `usr/bin/snapx` |
| **Fedora / RHEL** | `snapx-*.x86_64.rpm` | `sudo dnf install ./snapx-*.rpm` |
| **Arch** | [`packaging/linux/PKGBUILD`](packaging/linux/PKGBUILD) | `makepkg -si` |
| **Flatpak** | bundle on Releases | `flatpak install io.github.snapx` *(Flathub submission pending)* |
| **Windows 10/11** | `snapx-*-win64-setup.exe` | Run the installer; launch **snapx** from Start |
| **macOS** | `snapx-*-macos.dmg` | Open the DMG, drag **snapx** to Applications |

> **macOS Gatekeeper:** release builds aren't notarized. On first launch, right-click the app → **Open**, or allow it in **System Settings → Privacy & Security**.

**Runtime requirements**

| Platform | Needs |
|----------|-------|
| Linux (Wayland) | `xdg-desktop-portal` + a backend (e.g. `xdg-desktop-portal-gnome`); PipeWire for the ScreenCast fallback |
| Linux (Wayland global hotkeys) | Portal **GlobalShortcuts** (GNOME 45+ / KDE Plasma 6+); snapx installed so `snapx` and `io.github.snapx.desktop` are on `PATH` |
| Linux (X11) | X11 session with RandR |
| Windows | Windows 10+ (DXGI; GDI fallback on Windows 8) |
| macOS | macOS 10.13+ (ScreenCaptureKit on 12.3+) |

---

## Quick start

### GUI

```bash
snapx
```

1. Click **Screen**, **Region**, **Monitor**, or **Window** in the header bar.
2. In region mode, drag on the frozen overlay; **Enter** confirms, **Escape** cancels.
3. Annotate, optionally click **Beautify…**, then **Save** (`Ctrl+S`) or **Copy** (`Ctrl+C`).

Default global hotkey: **`Super+Shift+S`**. On Wayland, approve global shortcuts in the portal dialog the first time you run snapx.

### CLI (headless)

```bash
snapx -m fullscreen -n -o ~/Pictures/shot.png        # full screen → file
snapx -m region                                       # region (opens overlay)
snapx -m active -c -o ~/Pictures/window.png           # active window → clipboard + file
snapx -m monitor -M 1 -f jpeg -q 85 -n -o mon1.jpg    # monitor 1, JPEG q85
snapx -n -m region -o /tmp/s.png --upload --copy-url  # capture, upload, copy URL
```

<details>
<summary><b>All CLI options</b></summary>

| Option | Description |
|--------|-------------|
| `-m, --mode` | `fullscreen` \| `monitor` \| `region` \| `window` \| `active` |
| `-d, --delay` | Pre-capture delay in seconds (0–60) |
| `-M, --monitor` | Monitor index (0-based), `-1` = all |
| `-o, --output` | Output file path (enables headless when set) |
| `-f, --format` | `png` \| `jpeg` \| `webp` |
| `-q, --quality` | JPEG quality 1–100 (default 90) |
| `-c, --clipboard` | Also copy to clipboard |
| `--upload` | Upload after capture (uses `[upload]` config; needs libcurl) |
| `--copy-url` | Copy the upload URL to clipboard (with `--upload`) |
| `--background` | Start hidden; keep running for hotkeys/tray |
| `-n, --no-gui` | Capture without showing the main window |
| `-v, --version` / `-h, --help` | Version / help |

</details>

### D-Bus automation

While the GUI runs, snapx registers **`io.github.snapx`** on the session bus:

```bash
gdbus call --session -d io.github.snapx -o /io/github/snapx -m io.github.snapx.CaptureRegion
gdbus call --session -d io.github.snapx -o /io/github/snapx -m io.github.snapx.GetLastPath
gdbus call --session -d io.github.snapx -o /io/github/snapx -m io.github.snapx.Save s "$HOME/Pictures/out.png"
```

---

## Features

| Area | What you get |
|------|--------------|
| **Capture** | Full screen, single monitor, dragged region, active window, delayed capture, optional cursor |
| **Multi-monitor** | Enumerates monitors; region selection spans edges with correct HiDPI slicing and per-monitor 1:1 freeze overlay |
| **Annotation** | Rectangle, ellipse, line, arrow, pen, text, highlight, numbered callout, blur/pixelate, redaction, eyedropper; undo/redo |
| **Beautiful export** | Padding, solid/gradient/transparent background, rounded corners, drop shadow — live preview, copy or save |
| **Region overlay** | Magnifier loupe (hold **Space**), snap-to-window highlight |
| **OCR** | Copy text from a capture (optional Tesseract; language in Settings) |
| **Share** | Imgur or custom POST endpoint; copy link after upload (optional libcurl) |
| **Workflow** | Pin to screen, post-capture crop, recents sidebar, reveal-in-folder, auto-copy after capture |
| **Output** | PNG, JPEG (quality 1–100), WebP; configurable filename patterns with date/counter tokens |
| **Background** | System tray, close-to-tray, `--background`, autostart desktop entry |
| **Automation** | Headless CLI and session D-Bus API |

### ✨ Beautiful export

Click **Beautify…** in the action bar to open a live editor:

- **Padding** around the screenshot.
- **Background**: a flat colour, a two-colour **gradient**, or **transparent** (PNG/WebP).
- **Rounded corners** on the screenshot.
- A soft **drop shadow** with adjustable spread.

Toggle **Apply on Save/Copy/Upload** to make every export use these settings automatically — one click to share-ready images. Settings persist in your config.

### Annotation tools

`Rect` · `Ellipse` · `Line` · `Arrow` · `Pen` · `Text` · `Blur` · `Hi-lite` · `Callout` · `Redact` — pick a colour, or use **Pick** (eyedropper) to sample one from the image. Everything is undoable (`Ctrl+Z` / `Ctrl+Y`).

---

## Platform & Wayland notes

| OS | Session / API | Capture backend | GUI |
|----|---------------|-----------------|-----|
| Linux | Wayland + portal | XDG Screenshot (default); optional ScreenCast + PipeWire | GTK 4 / GTK 3 |
| Linux | X11 | libX11 + XRandR + XFixes | GTK 4 / GTK 3 |
| Windows 10/11 | — | DXGI Desktop Duplication | GTK 4 (MSYS2) |
| Windows 8 | — | GDI BitBlt | GTK 4 |
| macOS 12.3+ | — | ScreenCaptureKit | GTK 4 |
| macOS 10.13–12.2 | — | CoreGraphics | GTK 3 / 4 |

Third-party apps cannot call GNOME Shell's private screenshot APIs, so on Wayland snapx uses the standard **XDG Desktop Portal** stack — the same path as the built-in Print Screen. Region capture takes one full-desktop frame, then shows a native 1:1 freeze on each monitor so selection stays aligned on HiDPI and multi-monitor layouts. Capture runs on a background thread to keep the UI responsive.

```ini
# ~/.config/snapx/config.ini
[capture]
wayland_capture_prefer = screenshot   ; or "screencast" (one-frame fallback)
```

**Global hotkeys on Wayland** use `org.freedesktop.portal.GlobalShortcuts` (not raw key grabs): install snapx so `snapx` is on `PATH`, launch it from the app menu, and approve the shortcuts in the portal dialog on first run. On X11 and Windows, global hotkeys use native grab APIs and need no portal.

---

## Configuration

| Platform | Config path |
|----------|-------------|
| Linux | `~/.config/snapx/config.ini` |
| macOS | `~/Library/Application Support/snapx/config.ini` |
| Windows | `%APPDATA%\snapx\config.ini` |

Everything is editable in **Settings**, or directly in the INI file. Filename patterns support date tokens (`%Y %m %d %H %M %S`), an auto-increment counter (`%n`, or `%d`/`%03d` for unpadded/zero-padded), and `%%` for a literal percent — counters scan the save directory for the next free number.

### Keyboard shortcuts (defaults)

| Shortcut | Action | | Shortcut | Action |
|----------|--------|---|----------|--------|
| `Super+Shift+S` | Global capture (default mode) | | `Ctrl+S` | Save |
| `Ctrl+Shift+1` | Full screen | | `Ctrl+C` | Copy to clipboard |
| `Ctrl+Shift+2` | Monitor | | `Ctrl+Z` / `Ctrl+Y` | Undo / redo |
| `Ctrl+Shift+3` | Region | | `Ctrl+0` | Fit view |
| `Ctrl+Shift+4` | Active window | | `Ctrl +` / `Ctrl -` | Zoom in / out |
| `Return` | Confirm region | | `Escape` | Cancel region |

All shortcuts are remappable in **Settings → Shortcuts**.

---

## Building from source

### Prerequisites

<details open>
<summary><b>Fedora</b></summary>

```bash
sudo dnf install cmake gcc gtk4-devel cairo-devel glib2-devel gdk-pixbuf2-devel \
    libX11-devel libXrandr-devel libXfixes-devel libpng-devel libjpeg-turbo-devel \
    libwebp-devel pipewire-devel dbus-devel librsvg2
```
</details>

<details>
<summary><b>Ubuntu / Debian</b></summary>

```bash
sudo apt install cmake gcc libgtk-4-dev libcairo2-dev libglib2.0-dev \
    libgdk-pixbuf-2.0-dev libx11-dev libxrandr-dev libxfixes-dev \
    libpng-dev libjpeg-turbo8-dev libwebp-dev libpipewire-0.3-dev libdbus-1-dev librsvg2-bin
```
</details>

<details>
<summary><b>Arch / macOS / Windows</b></summary>

- **Arch:** `sudo pacman -S cmake gcc gtk4 cairo glib2 gdk-pixbuf2 libx11 libxrandr libxfixes libpng libjpeg-turbo libwebp pipewire dbus librsvg`
- **macOS:** `brew install cmake gtk4 cairo glib gdk-pixbuf libpng libjpeg-turbo webp`
- **Windows (MSYS2 UCRT64):** see [`packaging/windows/build-installer.sh`](packaging/windows/build-installer.sh)
</details>

Optional features are auto-detected: **libcurl** enables upload, **Tesseract** enables OCR, **libwebp** enables WebP output.

### Build

```bash
git clone https://github.com/Prem01-cyber/snapx.git
cd snapx
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/snapx --version
sudo cmake --install build        # optional
```

### Packaging

```bash
./packaging/linux/build-appimage.sh 2.0.1      # AppImage + tarball
./packaging/linux/build-rpm.sh 2.0.1           # Fedora/RHEL RPM
./packaging/macos/build-dmg.sh 2.0.1           # macOS
# Windows (MSYS2): ./packaging/windows/build-installer.sh 2.0.1
```

Tagging `vX.Y.Z` triggers [`.github/workflows/release.yml`](.github/workflows/release.yml), which builds and uploads the AppImage, Windows installer, and macOS DMG to the GitHub Release.

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| Stuck at `capturing frame…` (PipeWire) | Set `wayland_capture_prefer = screenshot` |
| Portal permission denied | Allow screen capture in system settings |
| Global hotkeys don't fire on Wayland | Install snapx, launch from the app menu, approve the portal dialog |
| AppImage won't run | `chmod +x` it; ensure FUSE / `libfuse2` on older distros |
| `VK_SUBOPTIMAL_KHR` Gdk-WARNING | Harmless Vulkan resize warning; ignore |

Run `snapx --info` to print the detected session, backend, and capability status.

---

## Project layout

```
snapx/
├── src/          Application source
│   ├── capture/    Platform capture backends (X11, Wayland, Windows, macOS)
│   ├── ui/         GTK windows, toolbar, dialogs, tray, history
│   ├── annotation/ Cairo annotation canvas + drawing
│   ├── output/     Save/encode, clipboard, upload, OCR, beautify
│   └── utils/      Config, shortcuts, hotkeys, monitors
├── resources/    Icons, CSS, GResource bundle
├── packaging/    AppImage, RPM, PKGBUILD, Windows installer, macOS DMG
└── .github/      CI and release workflows
```

---

## Contributing

Issues and pull requests are welcome on [GitHub](https://github.com/Prem01-cyber/snapx/issues). Please keep changes warning-clean (`-Wall -Wextra -Werror`) and test on at least one platform.

## License

Released under the **MIT License** — Copyright © 2026 the snapx authors. See [LICENSE](LICENSE).

## Acknowledgements

Built with GTK, Cairo, XDG Desktop Portals, PipeWire, and platform-native capture APIs — for a small, honest screenshot tool on modern desktops.
