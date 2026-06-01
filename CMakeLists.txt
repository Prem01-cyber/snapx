cmake_minimum_required(VERSION 3.16)
project(snapx VERSION 2.0.1 LANGUAGES C)

if(APPLE)
    enable_language(OBJC)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ─── Build type default ───────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SNAPX_ENABLE_ASAN "Enable AddressSanitizer (Debug only; requires libasan)" OFF)

# ─── Compiler flags ───────────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(/W4 /WX /utf-8)
else()
    add_compile_options(-Wall -Wextra -Werror -Wno-unused-parameter)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-g -O0)
        if(SNAPX_ENABLE_ASAN)
            include(CheckCSourceCompiles)
            set(CMAKE_REQUIRED_FLAGS "-fsanitize=address")
            check_c_source_compiles("int main(void){return 0;}" SNAPX_HAVE_ASAN)
            unset(CMAKE_REQUIRED_FLAGS)
            if(SNAPX_HAVE_ASAN)
                add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
                add_link_options(-fsanitize=address)
                message(STATUS "AddressSanitizer enabled")
            else()
                message(WARNING "SNAPX_ENABLE_ASAN=ON but libasan unavailable — building without sanitizer")
            endif()
        endif()
    else()
        add_compile_options(-O2)
    endif()
endif()

# ─── Platform detection ───────────────────────────────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(PLATFORM_LINUX TRUE)
    add_compile_definitions(SNAPX_PLATFORM_LINUX)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(PLATFORM_WINDOWS TRUE)
    add_compile_definitions(SNAPX_PLATFORM_WINDOWS)
    add_compile_definitions(WINVER=0x0601 _WIN32_WINNT=0x0601)  # Windows 7+
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(PLATFORM_MACOS TRUE)
    add_compile_definitions(SNAPX_PLATFORM_MACOS)
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

# ─── pkg-config ───────────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)

# ─── GTK detection (GTK4 preferred, GTK3 fallback) ───────────────────────────
option(SNAPX_HEADLESS_ONLY "Build without GTK (CLI/headless only)" OFF)

# PipeWire/SPA headers require POSIX extended locale APIs
add_compile_definitions(_GNU_SOURCE)

pkg_check_modules(GTK4 gtk4)
if(GTK4_FOUND)
    message(STATUS "Using GTK4")
    add_compile_definitions(SNAPX_USE_GTK4)
    set(GTK_LIBRARIES    ${GTK4_LIBRARIES})
    set(GTK_INCLUDE_DIRS ${GTK4_INCLUDE_DIRS})
    set(GTK_CFLAGS_OTHER ${GTK4_CFLAGS_OTHER})
    set(SNAPX_HAVE_GTK TRUE)
else()
    pkg_check_modules(GTK3 gtk+-3.0)
    if(GTK3_FOUND)
        message(STATUS "GTK4 not found, falling back to GTK3")
        add_compile_definitions(SNAPX_USE_GTK3)
        set(GTK_LIBRARIES    ${GTK3_LIBRARIES})
        set(GTK_INCLUDE_DIRS ${GTK3_INCLUDE_DIRS})
        set(GTK_CFLAGS_OTHER ${GTK3_CFLAGS_OTHER})
        set(SNAPX_HAVE_GTK TRUE)
    else()
        if(NOT SNAPX_HEADLESS_ONLY)
            message(FATAL_ERROR
                "Neither GTK4 nor GTK3 development libraries were found.\n"
                "Please install one of:\n"
                "  Fedora:  sudo dnf install gtk4-devel\n"
                "  Ubuntu:  sudo apt install libgtk-4-dev\n"
                "  Arch:    sudo pacman -S gtk4\n"
                "  macOS:   brew install gtk4\n"
                "Or configure with -DSNAPX_HEADLESS_ONLY=ON for CLI-only build.")
        else()
            message(WARNING "Building headless (no GTK) — GUI features disabled.")
            add_compile_definitions(SNAPX_HEADLESS)
            set(GTK_LIBRARIES    "")
            set(GTK_INCLUDE_DIRS "")
            set(GTK_CFLAGS_OTHER "")
        endif()
    endif()
endif()

# ─── Cairo ────────────────────────────────────────────────────────────────────
if(SNAPX_HEADLESS_ONLY)
    pkg_check_modules(CAIRO cairo)
else()
    pkg_check_modules(CAIRO REQUIRED cairo)
endif()
if(CAIRO_FOUND)
    add_compile_definitions(SNAPX_HAVE_CAIRO)
endif()

# ─── GLib / GIO (DBus, settings) ─────────────────────────────────────────────
if(SNAPX_HEADLESS_ONLY)
    pkg_check_modules(GLIB glib-2.0)
    pkg_check_modules(GIO  gio-2.0)
else()
    pkg_check_modules(GLIB REQUIRED glib-2.0)
    pkg_check_modules(GIO  REQUIRED gio-2.0)
endif()

# ─── GdkPixbuf ────────────────────────────────────────────────────────────────
if(SNAPX_HEADLESS_ONLY)
    pkg_check_modules(GDK_PIXBUF gdk-pixbuf-2.0)
else()
    pkg_check_modules(GDK_PIXBUF REQUIRED gdk-pixbuf-2.0)
endif()
if(GDK_PIXBUF_FOUND)
    add_compile_definitions(SNAPX_HAVE_GDK_PIXBUF)
endif()

# ─── libpng / libjpeg / libwebp ───────────────────────────────────────────────
find_package(PNG REQUIRED)
find_package(JPEG)
if(NOT JPEG_FOUND)
    message(WARNING "libjpeg not found — JPEG output disabled")
else()
    add_compile_definitions(SNAPX_HAVE_JPEG)
endif()
pkg_check_modules(WEBP libwebp)
if(WEBP_FOUND)
    add_compile_definitions(SNAPX_HAVE_WEBP)
endif()

pkg_check_modules(CURL libcurl)
if(CURL_FOUND)
    add_compile_definitions(SNAPX_HAVE_UPLOAD)
    message(STATUS "libcurl found — upload support enabled")
else()
    message(STATUS "libcurl not found — upload support disabled")
endif()

pkg_check_modules(TESSERACT tesseract lept)
if(TESSERACT_FOUND)
    add_compile_definitions(SNAPX_HAVE_TESSERACT)
    message(STATUS "Tesseract found — OCR support enabled")
endif()

# ─── Platform-specific libraries ─────────────────────────────────────────────
if(PLATFORM_LINUX)
    pkg_check_modules(X11    x11)
    pkg_check_modules(XRANDR xrandr)
    pkg_check_modules(XFIXES xfixes)
    pkg_check_modules(PIPEWIRE libpipewire-0.3)
    pkg_check_modules(DBUS   dbus-1)

    if(X11_FOUND AND XRANDR_FOUND)
        add_compile_definitions(SNAPX_HAVE_X11)
        message(STATUS "X11 + XRandR backend enabled")
    else()
        message(STATUS "X11/XRandR not found — X11 backend disabled")
    endif()

    if(PIPEWIRE_FOUND AND DBUS_FOUND)
        add_compile_definitions(SNAPX_HAVE_WAYLAND)
        message(STATUS "PipeWire + DBus found — Wayland backend enabled")
    else()
        message(STATUS "PipeWire/DBus not fully found — Wayland backend may be limited")
        # Still define so the portal-only path compiles
        add_compile_definitions(SNAPX_HAVE_WAYLAND)
    endif()
endif()

if(PLATFORM_MACOS)
    find_library(COREGRAPHICS_LIB   CoreGraphics   REQUIRED)
    find_library(COREFOUNDATION_LIB CoreFoundation REQUIRED)
    find_library(APPKIT_LIB         AppKit         REQUIRED)
    find_library(IOKIT_LIB          IOKit          REQUIRED)
    find_library(SCREENCAPTUREKIT   ScreenCaptureKit)
    if(SCREENCAPTUREKIT)
        add_compile_definitions(SNAPX_HAVE_SCK)
        message(STATUS "ScreenCaptureKit found — macOS 12.3+ capture enabled")
    endif()
endif()

if(PLATFORM_WINDOWS)
    # DXGI for Windows 10+ (header only, links via d3d11/dxgi libs)
    find_library(DXGI_LIB dxgi)
    if(DXGI_LIB)
        add_compile_definitions(SNAPX_HAVE_DXGI)
    endif()
endif()

# ─── Source files ─────────────────────────────────────────────────────────────
set(SOURCES
    src/main.c
    src/capture/capture.c
    src/output/save.c
    src/output/beautify.c
    src/output/clipboard.c
    src/output/upload.c
    src/utils/config.c
    src/utils/pattern.c
    src/utils/shortcut.c
    src/utils/monitor.c
    src/utils/hotkey.c
)

if(PLATFORM_LINUX)
    list(APPEND SOURCES src/utils/hotkey_wayland.c src/utils/portal_host.c)
endif()

if(NOT SNAPX_HEADLESS_ONLY)
    list(APPEND SOURCES
        src/ui/window_main.c
        src/ui/overlay.c
        src/ui/toolbar.c
        src/ui/beautify_dialog.c
        src/ui/settings_dialog.c
        src/ui/tray.c
        src/ui/pin_window.c
        src/ui/history_panel.c
        src/output/ocr.c
        src/ipc/dbus_service.c
        src/annotation/draw.c
        src/annotation/canvas.c
    )
endif()

# Platform detection module (always included)
list(APPEND SOURCES src/capture/platform.c)

# Platform capture backends
if(PLATFORM_LINUX)
    list(APPEND SOURCES src/capture/capture_x11.c)
    list(APPEND SOURCES src/capture/capture_wayland.c)
elseif(PLATFORM_WINDOWS)
    list(APPEND SOURCES src/capture/capture_windows.c)
elseif(PLATFORM_MACOS)
    list(APPEND SOURCES src/capture/capture_macos.c)
    if(SCREENCAPTUREKIT)
        set_source_files_properties(src/capture/capture_macos.c PROPERTIES LANGUAGE OBJC)
    endif()
endif()

# ─── Resources (GLib resource bundle) ────────────────────────────────────────
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources)
if(GLIB_COMPILE_RESOURCES)
    # Track the files referenced by the manifest (e.g. snapx.css) so editing
    # them re-bundles the GResource — not just editing the .xml itself.
    execute_process(
        COMMAND ${GLIB_COMPILE_RESOURCES} --generate-dependencies
                --sourcedir=${CMAKE_CURRENT_SOURCE_DIR}/resources
                ${CMAKE_CURRENT_SOURCE_DIR}/resources/snapx.gresource.xml
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/resources
        OUTPUT_VARIABLE SNAPX_RESOURCE_DEPS
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(REPLACE "\n" ";" SNAPX_RESOURCE_DEPS "${SNAPX_RESOURCE_DEPS}")
    set(SNAPX_RESOURCE_DEP_PATHS "")
    foreach(dep ${SNAPX_RESOURCE_DEPS})
        if(IS_ABSOLUTE "${dep}")
            list(APPEND SNAPX_RESOURCE_DEP_PATHS "${dep}")
        else()
            list(APPEND SNAPX_RESOURCE_DEP_PATHS
                 "${CMAKE_CURRENT_SOURCE_DIR}/resources/${dep}")
        endif()
    endforeach()
    add_custom_command(
        OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/snapx_resources.c
        COMMAND ${GLIB_COMPILE_RESOURCES}
                --target=${CMAKE_CURRENT_BINARY_DIR}/snapx_resources.c
                --generate-source
                ${CMAKE_CURRENT_SOURCE_DIR}/resources/snapx.gresource.xml
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/resources/snapx.gresource.xml
                ${SNAPX_RESOURCE_DEP_PATHS}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/resources
        COMMENT "Compiling GLib resources"
    )
    list(APPEND SOURCES ${CMAKE_CURRENT_BINARY_DIR}/snapx_resources.c)
    add_compile_definitions(SNAPX_HAVE_RESOURCES)
endif()

# ─── Executable ───────────────────────────────────────────────────────────────
if(PLATFORM_WINDOWS)
    add_executable(snapx WIN32 ${SOURCES})
else()
    add_executable(snapx ${SOURCES})
endif()

target_compile_definitions(snapx PRIVATE
    SNAPX_VERSION_STR="${PROJECT_VERSION}")

# ─── Include directories ──────────────────────────────────────────────────────
target_include_directories(snapx PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${GTK_INCLUDE_DIRS}
    ${CAIRO_INCLUDE_DIRS}
    ${GLIB_INCLUDE_DIRS}
    ${GIO_INCLUDE_DIRS}
    ${GDK_PIXBUF_INCLUDE_DIRS}
    ${PNG_INCLUDE_DIRS}
)

if(PLATFORM_LINUX)
    if(X11_FOUND)
        target_include_directories(snapx PRIVATE ${X11_INCLUDE_DIRS} ${XRANDR_INCLUDE_DIRS})
    endif()
    if(PIPEWIRE_FOUND)
        target_compile_definitions(snapx PRIVATE SNAPX_HAVE_PIPEWIRE)
        target_include_directories(snapx PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
    endif()
    if(DBUS_FOUND)
        target_include_directories(snapx PRIVATE ${DBUS_INCLUDE_DIRS})
    endif()
endif()

if(WEBP_FOUND)
    target_include_directories(snapx PRIVATE ${WEBP_INCLUDE_DIRS})
endif()

# ─── Link libraries ───────────────────────────────────────────────────────────
target_link_libraries(snapx PRIVATE
    ${GTK_LIBRARIES}
    ${CAIRO_LIBRARIES}
    ${GLIB_LIBRARIES}
    ${GIO_LIBRARIES}
    ${GDK_PIXBUF_LIBRARIES}
    PNG::PNG
)
if(JPEG_FOUND)
    target_link_libraries(snapx PRIVATE ${JPEG_LIBRARIES})
    target_include_directories(snapx PRIVATE ${JPEG_INCLUDE_DIRS})
endif()

if(WEBP_FOUND)
    target_link_libraries(snapx PRIVATE ${WEBP_LIBRARIES})
endif()

if(CURL_FOUND)
    target_include_directories(snapx PRIVATE ${CURL_INCLUDE_DIRS})
    target_link_libraries(snapx PRIVATE ${CURL_LIBRARIES})
    target_compile_options(snapx PRIVATE ${CURL_CFLAGS_OTHER})
endif()

if(TESSERACT_FOUND)
    target_include_directories(snapx PRIVATE ${TESSERACT_INCLUDE_DIRS})
    target_link_libraries(snapx PRIVATE ${TESSERACT_LIBRARIES})
    target_compile_options(snapx PRIVATE ${TESSERACT_CFLAGS_OTHER})
endif()

if(PLATFORM_LINUX)
    if(X11_FOUND)
        target_link_libraries(snapx PRIVATE ${X11_LIBRARIES} ${XRANDR_LIBRARIES})
    endif()
    if(XFIXES_FOUND)
        target_link_libraries(snapx PRIVATE ${XFIXES_LIBRARIES})
    endif()
    if(PIPEWIRE_FOUND)
        target_link_libraries(snapx PRIVATE ${PIPEWIRE_LIBRARIES})
    endif()
    if(DBUS_FOUND)
        target_link_libraries(snapx PRIVATE ${DBUS_LIBRARIES})
    endif()
    target_link_libraries(snapx PRIVATE m)
endif()

if(PLATFORM_WINDOWS)
    target_link_libraries(snapx PRIVATE
        user32 gdi32 dwmapi ole32 shell32 comctl32 shlwapi
    )
    if(DXGI_LIB)
        target_link_libraries(snapx PRIVATE dxgi d3d11)
    endif()
endif()

if(PLATFORM_MACOS)
    if(GTK4_FOUND AND GTK4_LIBRARY_DIRS)
        target_link_directories(snapx PRIVATE ${GTK4_LIBRARY_DIRS})
    endif()
    target_link_libraries(snapx PRIVATE
        ${COREGRAPHICS_LIB}
        ${COREFOUNDATION_LIB}
        ${APPKIT_LIB}
        ${IOKIT_LIB}
    )
    if(SCREENCAPTUREKIT)
        target_link_libraries(snapx PRIVATE ${SCREENCAPTUREKIT})
    endif()
endif()

# ─── Compile definitions (flags) ──────────────────────────────────────────────
target_compile_options(snapx PRIVATE ${GTK_CFLAGS_OTHER} ${CAIRO_CFLAGS_OTHER})

# ─── Install ──────────────────────────────────────────────────────────────────
install(TARGETS snapx RUNTIME DESTINATION bin)

if(PLATFORM_LINUX)
    install(FILES packaging/linux/io.github.snapx.desktop
            DESTINATION share/applications)
    install(FILES packaging/linux/snapx.desktop
            DESTINATION share/applications)
    install(FILES resources/icons/snapx.svg
            DESTINATION share/icons/hicolor/scalable/apps
            RENAME snapx.svg)
    install(DIRECTORY resources/icons/hicolor/
            DESTINATION share/icons/hicolor
            FILES_MATCHING PATTERN "*.png")
endif()

if(PLATFORM_MACOS)
    install(FILES packaging/macos/Info.plist
            DESTINATION ${CMAKE_INSTALL_PREFIX}/snapx.app/Contents)
endif()

# ─── Summary ──────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "snapx ${PROJECT_VERSION} configuration:")
message(STATUS "  Platform      : ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Build type    : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C compiler    : ${CMAKE_C_COMPILER}")
if(GTK4_FOUND)
message(STATUS "  GUI           : GTK4")
else()
message(STATUS "  GUI           : GTK3 (fallback)")
endif()
if(PLATFORM_LINUX)
message(STATUS "  X11 backend   : ${X11_FOUND}")
message(STATUS "  Wayland/PW    : ${PIPEWIRE_FOUND}")
endif()
message(STATUS "  WebP support  : ${WEBP_FOUND}")
message(STATUS "")
