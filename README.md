# snapx

**A lightweight, cross-platform screenshot utility written in C**

snapx captures, annotates, and saves screenshots on Linux (Wayland & X11),
Windows 7/8/10/11, and macOS 10.13+.

---

## Features

| Feature | Details |
|---|---|
| **Capture modes** | Full screen, single monitor, region selection, active window, delayed |
| **Annotation** | Rectangles, arrows, freehand pen, text, blur/pixelate, highlight |
| **Output formats** | PNG (lossless), JPEG (quality 1–100), WebP |
| **Clipboard** | Copy to clipboard (auto or manual) |
| **Multi-monitor** | Enumerate, capture single monitor or all combined |
| **GUI** | GTK4 (GTK3 fallback) with Cairo canvas |
| **Headless mode** | Full CLI (`--no-gui`) for scripting and automation |
| **Settings** | Persisted INI config with live preview in settings dialog |
| **Hotkeys** | Configurable global hotkey + in-app shortcuts |

---

## Platform support

| Platform | Capture backend | GUI |
|---|---|---|
| Linux (Wayland) | XDG Desktop Portal + PipeWire | GTK4 / GTK3 |
| Linux (X11/Xorg) | libX11 + libXRandR + libXfixes | GTK4 / GTK3 |
| Windows 10/11 | DXGI Desktop Duplication | GTK4 (via MSYS2) or Win32 |
| Windows 7/8 | GDI BitBlt | GTK3 (via MSYS2) or Win32 |
| macOS 12.3+ | ScreenCaptureKit | GTK4 (via Homebrew) or Cocoa |
| macOS 10.13–12.2 | CoreGraphics CGWindowList | GTK3 (via Homebrew) or Cocoa |

---

## Building

### Prerequisites

**Fedora / RHEL / CentOS:**
```bash
sudo dnf install cmake gcc gtk4-devel cairo-devel glib2-devel \
    gdk-pixbuf2-devel libX11-devel libXrandr-devel libXfixes-devel \
    libpng-devel libjpeg-turbo-devel libwebp-devel pipewire-devel dbus-devel
```

**Ubuntu / Debian:**
```bash
sudo apt install cmake gcc libgtk-4-dev libcairo2-dev libglib2.0-dev \
    libgdk-pixbuf-2.0-dev libx11-dev libxrandr-dev libxfixes-dev \
    libpng-dev libjpeg-turbo8-dev libwebp-dev libpipewire-0.3-dev libdbus-1-dev
```

**Arch Linux:**
```bash
sudo pacman -S cmake gcc gtk4 cairo glib2 gdk-pixbuf2 \
    libx11 libxrandr libxfixes libpng libjpeg-turbo libwebp pipewire dbus
```

**macOS (Homebrew):**
```bash
brew install cmake gtk4 cairo glib gdk-pixbuf libpng libjpeg-turbo webp
```

**Windows (MSYS2 / MinGW-w64):**
```bash
pacman -S mingw-w64-x86_64-cmake mingw-w64-x86_64-gtk4 \
    mingw-w64-x86_64-cairo mingw-w64-x86_64-libpng \
    mingw-w64-x86_64-libjpeg-turbo mingw-w64-x86_64-libwebp
```

### Build

```bash
git clone https://github.com/snapx/snapx.git
cd snapx
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
# Optional install:
sudo cmake --install build
```

### Debug build with AddressSanitizer

```bash
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j$(nproc)
```

---

## Usage

### GUI mode
```bash
snapx                         # Open main window
```

### CLI / headless mode
```bash
# Full-screen capture → file
snapx -m fullscreen -n -o ~/screenshot.png

# Region capture (opens overlay for selection)
snapx -m region

# Active window → clipboard + file
snapx -m active -c -o window.png

# Delayed full-screen capture (5 second countdown)
snapx -m fullscreen -d 5 -n -o delayed.png

# JPEG capture of monitor index 1
snapx -m monitor -M 1 -f jpeg -q 85 -n -o monitor1.jpg
```

### All CLI options
```
-m, --mode <mode>        fullscreen | monitor | region | window | active
-d, --delay <sec>        Pre-capture delay in seconds (0–60)
-M, --monitor <index>    Monitor index (0-based), -1 = all
-o, --output <path>      Output file path (enables headless mode)
-f, --format <fmt>       png | jpeg | webp
-q, --quality <1-100>    JPEG quality (default: 90)
-c, --clipboard          Also copy to clipboard
-n, --no-gui             Headless: capture without showing window
-v, --version            Show version
-h, --help               Show help
```

---

## Keyboard shortcuts

| Shortcut | Action |
|---|---|
| `Ctrl+S` | Save screenshot |
| `Ctrl+C` | Copy to clipboard |
| `Ctrl+Z` | Undo annotation |
| `Ctrl+Y` | Redo annotation |
| `Escape` | Cancel overlay / close |
| `Enter` | Confirm region selection |

---

## Configuration

Settings are stored at:

- **Linux**: `~/.config/snapx/config.ini`
- **macOS**: `~/Library/Application Support/snapx/config.ini`
- **Windows**: `%APPDATA%\snapx\config.ini`

### Example `config.ini`

```ini
[save]
save_dir         = /home/user/Pictures/Screenshots
filename_pattern = screenshot_%Y%m%d_%H%M%S

[capture]
default_mode     = 0       # 0=fullscreen 1=monitor 2=region 3=window 4=active
default_delay    = 0
show_cursor      = 0

[output]
default_format   = 0       # 0=PNG 1=JPEG 2=WebP
jpeg_quality     = 90
auto_clipboard   = 0
play_sound       = 1

[annotation]
default_tool     = 0       # 0=rect 1=arrow 2=pen 3=text 4=blur 5=highlight
default_color_r  = 1.0000
default_color_g  = 0.2000
default_color_b  = 0.2000
default_color_a  = 1.0000

[hotkey]
hotkey           = super+shift+s

[window]
window_x         = 100
window_y         = 100
window_w         = 960
window_h         = 640
```

### Filename pattern tokens

| Token | Expands to |
|---|---|
| `%Y` | 4-digit year |
| `%m` | 2-digit month |
| `%d` | 2-digit day |
| `%H` | 2-digit hour (24h) |
| `%M` | 2-digit minute |
| `%S` | 2-digit second |
| `%n` | Auto-incremented counter (4 digits) |
| `%%` | Literal `%` |

---

## Annotation tools

| Tool | Description |
|---|---|
| **Rectangle** | Draws a coloured rectangle border |
| **Arrow** | Line with arrowhead at end point |
| **Pen** | Freehand smooth curve |
| **Text** | Click and type (uses active colour) |
| **Blur** | Pixelates a region to hide sensitive info |
| **Highlight** | Semi-transparent filled rectangle |

All tools use the active colour chosen from the colour picker in the toolbar.
Undo (`Ctrl+Z`) and redo (`Ctrl+Y`) are fully supported.

---

## Project structure

```
snapx/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── main.c                  Entry point, platform detection, arg parsing
│   ├── capture/
│   │   ├── capture.h           Unified capture interface
│   │   ├── capture.c           Common dispatch + image helpers
│   │   ├── capture_wayland.c   Wayland: XDG portal + GIO/DBus
│   │   ├── capture_x11.c       X11: libX11 + XRandR + XFixes
│   │   ├── capture_windows.c   Windows: GDI BitBlt + DXGI
│   │   └── capture_macos.c     macOS: CoreGraphics + ScreenCaptureKit
│   ├── ui/
│   │   ├── window_main.c/h     Main application window
│   │   ├── overlay.c/h         Full-screen region selection overlay
│   │   ├── toolbar.c/h         Annotation toolbar + canvas events
│   │   └── settings_dialog.c/h Settings dialog
│   ├── annotation/
│   │   ├── annotation.h        Tool types + draw primitives
│   │   ├── draw.c              Cairo rendering (all tool shapes)
│   │   ├── canvas.h            Canvas API declaration
│   │   └── canvas.c            Undo/redo stack, stroke lifecycle, flatten
│   ├── output/
│   │   ├── save.c/h            PNG / JPEG / WebP file saving
│   │   └── clipboard.c/h       System clipboard copy
│   └── utils/
│       ├── config.c/h          INI config read/write, path token expansion
│       ├── monitor.c/h         Multi-monitor enumeration helpers
│       └── hotkey.c/h          Global hotkey registration
├── resources/
│   ├── icons/                  App icons (SVG + PNG 16–256 px)
│   └── snapx.gresource.xml     GLib resource bundle descriptor
└── packaging/
    ├── linux/
    │   ├── snapx.spec           RPM spec (Fedora/RHEL)
    │   ├── snapx.desktop        FreeDesktop .desktop entry
    │   └── PKGBUILD             Arch Linux package
    ├── windows/
    │   └── installer.iss        Inno Setup installer script
    └── macos/
        └── Info.plist           macOS app bundle metadata
```

---

## License

MIT License — see [LICENSE](LICENSE) for details.
