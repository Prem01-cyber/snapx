/**
 * @file hotkey.c
 * @brief Global hotkey registration.
 *
 * Linux X11: XGrabKey on the root window, listened via GLib idle source.
 * Linux Wayland: No universal mechanism; we log a notice and skip gracefully.
 *                (Future: use libinput + udev with appropriate permissions.)
 * Windows: RegisterHotKey + message pump thread.
 * macOS: CGEventTap (requires Accessibility permission in System Preferences).
 */

#include "hotkey.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>   /* strcasecmp */

static SnapxHotkeyCallback g_hotkey_cb = NULL;

void snapx_hotkey_set_callback(SnapxHotkeyCallback cb)
{
    g_hotkey_cb = cb;
}

/* ─────────────────────────────────── Linux X11 ──────────────────────────── */
#if defined(SNAPX_PLATFORM_LINUX) && defined(SNAPX_HAVE_X11)

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <glib.h>
#include <pthread.h>

typedef struct {
    Display  *dpy;
    Window    root;
    int       keycode;
    unsigned  modifiers;
    int       stop;
} X11HotkeyState;

static X11HotkeyState g_x11hk = {0};

/**
 * @brief Parse a hotkey string like "super+shift+s" into X11 modifiers + keycode.
 */
static int parse_x11_hotkey(Display *dpy, const char *hotkey,
                              unsigned *mods_out, int *keycode_out)
{
    char buf[64]; snprintf(buf, sizeof(buf), "%s", hotkey);
    unsigned mods = 0;
    char *tok = strtok(buf, "+");
    KeySym sym = NoSymbol;

    while (tok) {
        if      (strcasecmp(tok, "super") == 0 || strcasecmp(tok, "mod4") == 0)
            mods |= Mod4Mask;
        else if (strcasecmp(tok, "ctrl")  == 0 || strcasecmp(tok, "control") == 0)
            mods |= ControlMask;
        else if (strcasecmp(tok, "alt")   == 0 || strcasecmp(tok, "mod1") == 0)
            mods |= Mod1Mask;
        else if (strcasecmp(tok, "shift") == 0)
            mods |= ShiftMask;
        else
            sym = XStringToKeysym(tok);
        tok = strtok(NULL, "+");
    }
    if (sym == NoSymbol) return 0;
    *mods_out    = mods;
    *keycode_out = XKeysymToKeycode(dpy, sym);
    return (*keycode_out != 0);
}

static int g_grab_failed = 0;
static int x11_grab_error_handler(Display *d, XErrorEvent *e)
{
    (void)d;
    if (e->error_code == BadAccess) g_grab_failed = 1;
    return 0;
}

static void *x11_hotkey_thread(void *arg)
{
    (void)arg;
    X11HotkeyState *st = &g_x11hk;

    while (!st->stop) {
        XEvent ev;
        if (XPending(st->dpy)) {
            XNextEvent(st->dpy, &ev);
            if (ev.type == KeyPress) {
                XKeyEvent *ke = &ev.xkey;
                if ((unsigned int)ke->keycode == (unsigned int)st->keycode &&
                    (ke->state & st->modifiers) == st->modifiers) {
                    if (g_hotkey_cb) g_hotkey_cb();
                }
            }
        } else {
            struct timespec ts = { 0, 20 * 1000 * 1000 }; /* 20ms */
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

void snapx_hotkey_init(SnapxConfig *config)
{
    if (!config || config->hotkey[0] == '\0') return;

    g_x11hk.dpy = XOpenDisplay(NULL);
    if (!g_x11hk.dpy) {
        fprintf(stderr, "[hotkey] X11 not available, hotkey disabled.\n");
        return;
    }

    unsigned mods = 0; int kc = 0;
    if (!parse_x11_hotkey(g_x11hk.dpy, config->hotkey, &mods, &kc)) {
        fprintf(stderr, "[hotkey] Could not parse hotkey '%s'\n", config->hotkey);
        XCloseDisplay(g_x11hk.dpy); g_x11hk.dpy = NULL; return;
    }

    g_x11hk.root     = DefaultRootWindow(g_x11hk.dpy);
    g_x11hk.modifiers = mods;
    g_x11hk.keycode   = kc;

    /* Suppress BadAccess — another app may already hold the grab */
    g_grab_failed = 0;
    int (*prev_err)(Display *, XErrorEvent *) =
        XSetErrorHandler(x11_grab_error_handler);

    XSelectInput(g_x11hk.dpy, g_x11hk.root, KeyPressMask);
    XGrabKey(g_x11hk.dpy, kc, mods, g_x11hk.root,
             True, GrabModeAsync, GrabModeAsync);
    XSync(g_x11hk.dpy, False);
    XSetErrorHandler(prev_err);

    if (g_grab_failed) {
        fprintf(stderr, "[hotkey] Could not grab '%s' (key already grabbed by another app). "
                        "Hotkey disabled — use in-app shortcuts instead.\n",
                config->hotkey);
        XCloseDisplay(g_x11hk.dpy);
        g_x11hk.dpy = NULL;
        return;
    }

    pthread_t tid;
    pthread_create(&tid, NULL, x11_hotkey_thread, NULL);
    pthread_detach(tid);
    fprintf(stderr, "[hotkey] Registered '%s' (X11 keycode=%d mod=0x%x)\n",
            config->hotkey, kc, mods);
}

void snapx_hotkey_cleanup(void)
{
    g_x11hk.stop = 1;
    if (g_x11hk.dpy) {
        XUngrabKey(g_x11hk.dpy, g_x11hk.keycode, g_x11hk.modifiers, g_x11hk.root);
        XCloseDisplay(g_x11hk.dpy);
        g_x11hk.dpy = NULL;
    }
}

#elif defined(SNAPX_PLATFORM_LINUX)

/* Wayland: no global grab mechanism without compositor cooperation */
void snapx_hotkey_init(SnapxConfig *config)
{
    (void)config;
    fprintf(stderr, "[hotkey] Global hotkeys on Wayland are not supported "
                    "(compositor restriction). Use in-app keyboard shortcuts.\n");
}
void snapx_hotkey_cleanup(void) {}

/* ─────────────────────────────────── Windows ────────────────────────────── */
#elif defined(SNAPX_PLATFORM_WINDOWS)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>

#define SNAPX_HOTKEY_ID 0xBEEF

static HWND  g_msg_hwnd  = NULL;
static HANDLE g_hk_thread = NULL;

static unsigned parse_win_modifiers(const char *hotkey)
{
    unsigned mods = 0;
    if (strstr(hotkey, "super") || strstr(hotkey, "win"))  mods |= MOD_WIN;
    if (strstr(hotkey, "ctrl")  || strstr(hotkey, "control")) mods |= MOD_CONTROL;
    if (strstr(hotkey, "alt")   || strstr(hotkey, "mod1")) mods |= MOD_ALT;
    if (strstr(hotkey, "shift"))                            mods |= MOD_SHIFT;
    return mods;
}

static unsigned int parse_win_vk(const char *hotkey)
{
    const char *p = strrchr(hotkey, '+');
    if (!p) p = hotkey;
    else    p++;
    if (strlen(p) == 1) return (unsigned int)toupper((unsigned char)*p);
    /* Special keys */
    if (strcasecmp(p, "print") == 0 || strcasecmp(p, "printscreen") == 0)
        return VK_SNAPSHOT;
    if (strcasecmp(p, "f12") == 0) return VK_F12;
    return 0;
}

static unsigned int __stdcall hotkey_thread(void *arg)
{
    (void)arg;
    /* Create a message-only window */
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = DefWindowProcW;
    wc.lpszClassName = L"SnapxHotkeyWnd";
    RegisterClassExW(&wc);
    g_msg_hwnd = CreateWindowExW(0, L"SnapxHotkeyWnd", NULL, 0,
                                  0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);

    unsigned mods = parse_win_modifiers((const char *)arg);
    unsigned vk   = parse_win_vk((const char *)arg);
    free(arg);

    if (vk == 0 || !RegisterHotKey(g_msg_hwnd, SNAPX_HOTKEY_ID, mods, vk)) {
        fprintf(stderr, "[hotkey] RegisterHotKey failed: %lu\n", GetLastError());
        return 0;
    }
    fprintf(stderr, "[hotkey] Global hotkey registered (Win32).\n");

    MSG msg;
    while (GetMessageW(&msg, g_msg_hwnd, 0, 0)) {
        if (msg.message == WM_HOTKEY && msg.wParam == SNAPX_HOTKEY_ID) {
            if (g_hotkey_cb) g_hotkey_cb();
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

void snapx_hotkey_init(SnapxConfig *config)
{
    if (!config || config->hotkey[0] == '\0') return;
    char *hk_copy = _strdup(config->hotkey);
    g_hk_thread = (HANDLE)_beginthreadex(NULL, 0, hotkey_thread, hk_copy, 0, NULL);
}

void snapx_hotkey_cleanup(void)
{
    if (g_msg_hwnd) {
        UnregisterHotKey(g_msg_hwnd, SNAPX_HOTKEY_ID);
        PostMessageW(g_msg_hwnd, WM_QUIT, 0, 0);
    }
    if (g_hk_thread) {
        WaitForSingleObject(g_hk_thread, 2000);
        CloseHandle(g_hk_thread);
    }
}

/* ─────────────────────────────────── macOS ──────────────────────────────── */
#elif defined(SNAPX_PLATFORM_MACOS)

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <pthread.h>

static CFMachPortRef g_event_tap = NULL;
static CFRunLoopSourceRef g_rl_source = NULL;
static pthread_t g_tap_thread = 0;
static CGKeyCode g_target_keycode = 0;
static CGEventFlags g_target_flags = 0;

static CGEventRef hotkey_tap_cb(CGEventTapProxy proxy, CGEventType type,
                                  CGEventRef event, void *refcon)
{
    (void)proxy; (void)refcon;
    if (type == kCGEventKeyDown) {
        CGKeyCode kc = (CGKeyCode)CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
        CGEventFlags flags = CGEventGetFlags(event) &
                             (kCGEventFlagMaskCommand | kCGEventFlagMaskShift |
                              kCGEventFlagMaskAlternate | kCGEventFlagMaskControl);
        if (kc == g_target_keycode && flags == g_target_flags) {
            if (g_hotkey_cb) g_hotkey_cb();
        }
    }
    return event;
}

static void *tap_run_loop(void *arg)
{
    (void)arg;
    CFRunLoopAddSource(CFRunLoopGetCurrent(), g_rl_source, kCFRunLoopCommonModes);
    CFRunLoopRun();
    return NULL;
}

void snapx_hotkey_init(SnapxConfig *config)
{
    (void)config;
    /* Requires Accessibility permission; failure is non-fatal */
    if (!AXIsProcessTrusted()) {
        fprintf(stderr, "[hotkey] Accessibility permission not granted. "
                        "Global hotkey disabled. "
                        "Enable in System Preferences → Security → Accessibility.\n");
        return;
    }

    /* Hardcoded: Super+Shift+S = Cmd+Shift+S for macOS */
    g_target_keycode = 1;  /* 's' key */
    g_target_flags   = kCGEventFlagMaskCommand | kCGEventFlagMaskShift;

    g_event_tap = CGEventTapCreate(
        kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
        CGEventMaskBit(kCGEventKeyDown), hotkey_tap_cb, NULL);

    if (!g_event_tap) {
        fprintf(stderr, "[hotkey] CGEventTapCreate failed.\n"); return;
    }

    g_rl_source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, g_event_tap, 0);
    pthread_create(&g_tap_thread, NULL, tap_run_loop, NULL);
    pthread_detach(g_tap_thread);
    fprintf(stderr, "[hotkey] CGEventTap registered (Cmd+Shift+S).\n");
}

void snapx_hotkey_cleanup(void)
{
    if (g_event_tap) {
        CGEventTapEnable(g_event_tap, FALSE);
        CFRelease(g_event_tap); g_event_tap = NULL;
    }
    if (g_rl_source) { CFRelease(g_rl_source); g_rl_source = NULL; }
}

#else

void snapx_hotkey_init(SnapxConfig *config) { (void)config; }
void snapx_hotkey_cleanup(void) {}

#endif
