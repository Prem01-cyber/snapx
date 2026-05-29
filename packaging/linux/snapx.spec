Name:           snapx
Version:        1.3.1
Release:        1%{?dist}
Summary:        Lightweight cross-platform screenshot utility
License:        MIT
URL:            https://github.com/Prem01-cyber/snapx
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.16
BuildRequires:  gcc
BuildRequires:  gtk4-devel
BuildRequires:  cairo-devel
BuildRequires:  glib2-devel
BuildRequires:  gdk-pixbuf2-devel
BuildRequires:  libX11-devel
BuildRequires:  libXrandr-devel
BuildRequires:  libXfixes-devel
BuildRequires:  libpng-devel
BuildRequires:  libjpeg-turbo-devel
BuildRequires:  libwebp-devel
BuildRequires:  pipewire-devel
BuildRequires:  dbus-devel
BuildRequires:  desktop-file-utils

Requires:       gtk4
Requires:       cairo
Requires:       glib2
Requires:       gdk-pixbuf2
Requires:       libpng
Requires:       libjpeg-turbo
Requires:       pipewire

%description
snapx is a lightweight, cross-platform screenshot utility with support for
Wayland (via XDG Desktop Portal), X11 (libX11 + XRandR), Windows (GDI/DXGI),
and macOS (CoreGraphics/ScreenCaptureKit).

Features:
  - Full screen, region, window, and monitor capture modes
  - Cairo-based annotation tools (rectangles, arrows, pen, text, blur, highlight)
  - PNG, JPEG, and WebP output
  - Configurable save directory and filename patterns
  - GTK4 GUI with GTK3 fallback

%prep
%autosetup

%build
%cmake \
    -DCMAKE_BUILD_TYPE=Release
%cmake_build

%install
%cmake_install
install -Dm644 packaging/linux/io.github.snapx.desktop \
    %{buildroot}%{_datadir}/applications/io.github.snapx.desktop
install -Dm644 packaging/linux/snapx.desktop \
    %{buildroot}%{_datadir}/applications/snapx.desktop
install -Dm644 resources/icons/16x16/apps/snapx.png \
    %{buildroot}%{_datadir}/icons/hicolor/16x16/apps/snapx.png
install -Dm644 resources/icons/32x32/apps/snapx.png \
    %{buildroot}%{_datadir}/icons/hicolor/32x32/apps/snapx.png
install -Dm644 resources/icons/48x48/apps/snapx.png \
    %{buildroot}%{_datadir}/icons/hicolor/48x48/apps/snapx.png
install -Dm644 resources/icons/64x64/apps/snapx.png \
    %{buildroot}%{_datadir}/icons/hicolor/64x64/apps/snapx.png
install -Dm644 resources/icons/128x128/apps/snapx.png \
    %{buildroot}%{_datadir}/icons/hicolor/128x128/apps/snapx.png
install -Dm644 resources/icons/256x256/apps/snapx.png \
    %{buildroot}%{_datadir}/icons/hicolor/256x256/apps/snapx.png
install -Dm644 resources/icons/snapx.svg \
    %{buildroot}%{_datadir}/icons/hicolor/scalable/apps/snapx.svg

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/io.github.snapx.desktop
desktop-file-validate %{buildroot}%{_datadir}/applications/snapx.desktop

%files
%license LICENSE
%doc README.md
%{_bindir}/snapx
%{_datadir}/applications/io.github.snapx.desktop
%{_datadir}/applications/snapx.desktop
%{_datadir}/icons/hicolor/*/apps/snapx.*

%changelog
* Fri May 29 2026 snapx Team <snapx@example.com> - 1.3.1-1
- Windows upload.c build fix; Flatpak CI --user install

* Fri May 29 2026 snapx Team <snapx@example.com> - 1.3.0-1
- Upload/OCR, blur fix, recents file list, tray, D-Bus, Flatpak CI

* Fri May 29 2026 snapx Team <snapx@example.com> - 1.2.1-1
- Settings UI fixes, color picker crash fix, Debug build without libasan

* Fri May 29 2026 snapx Team <snapx@example.com> - 1.2.0-1
- Wayland global hotkeys via xdg-desktop-portal GlobalShortcuts

* %(date "+%a %b %d %Y") snapx Team <snapx@example.com> - 1.1.0-1
- Settings UI, capture shortcuts, filename patterns, stability fixes

* Mon May 29 2026 snapx Team <snapx@example.com> - 1.0.0-1
- Initial package release
