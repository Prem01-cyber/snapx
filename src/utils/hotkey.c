/**
 * @file hotkey.c
 * @brief Global hotkey registration.
 *
 * Linux X11: XGrabKey on the root window, listened via background thread.
 * Linux Wayland: No universal mechanism; we log a notice and skip gracefully.
 * Windows: RegisterHotKey + message pump thread.
 * macOS: CGEventTap (requires Accessibility permission in System Preferences).
 */

#include "hotkey.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>

#define SNAPX_MAX_HOTKEY_GRABS 8

typedef struct {
    int               keycode;
    unsigned          modifiers;
    SnapxHotkeyAction action;
} HotkeyGrab;

static SnapxHotkeyCallback g_hotkey_cb = NULL;
static gpointer            g_hotkey_user_data = NULL;

void snapx_hotkey_set_callback(SnapxHotkeyCallback cb, gpointer user_data)
{
    g_hotkey_cb         = cb;
    g_hotkey_user_data  = user_data;
}

/* ─────────────────────────────────── Linux X11 ──────────────────────────── */
#if defined(SNAPX_PLATFORM_LINUX) && defined(SNAPX_HAVE_X11)

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <glib.h>
#include <pthread.h>

typedef struct {
    Display    *dpy;
    Window      root;
    HotkeyGrab  grabs[SNAPX_MAX_HOTKEY_GRABS];
    int         n_grabs;
    int         stop;
} X11HotkeyState;

static X11HotkeyState g_x11hk = {0};

static int parse_x11_hotkey(Display *dpy, const char *hotkey,
                              unsigned *mods_out, int *keycode_out)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", hotkey);
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
        else if (strcasecmp(tok, "plus") == 0 || strcasecmp(tok, "equal") == 0)
            sym = XK_plus;
        else if (strcasecmp(tok, "minus") == 0)
            sym = XK_minus;
        else if (strlen(tok) == 1 && tok[0] >= '0' && tok[0] <= '9')
            sym = XStringToKeysym(tok);
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
                for (int i = 0; i < st->n_grabs; i++) {
                    if ((unsigned int)ke->keycode == (unsigned int)st->grabs[i].keycode &&
                        (ke->state & st->grabs[i].modifiers) == st->grabs[i].modifiers) {
                        if (g_hotkey_cb)
                            g_hotkey_cb(st->grabs[i].action, g_hotkey_user_data);
                        break;
                    }
                }
            }
        } else {
            struct timespec ts = { 0, 20 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

static int grab_spec_dup(const HotkeyGrab *grabs, int n, int kc, unsigned mods)
{
    for (int i = 0; i < n; i++) {
        if (grabs[i].keycode == kc && grabs[i].modifiers == mods)
            return 1;
    }
    return 0;
}

static int x11_add_grab(Display *dpy, const char *spec, SnapxHotkeyAction action)
{
    if (!spec || !spec[0] || g_x11hk.n_grabs >= SNAPX_MAX_HOTKEY_GRABS)
        return 0;

    unsigned mods = 0;
    int kc = 0;
    if (!parse_x11_hotkey(dpy, spec, &mods, &kc))
        return 0;
    if (grab_spec_dup(g_x11hk.grabs, g_x11hk.n_grabs, kc, mods))
        return 0;

    HotkeyGrab *g = &g_x11hk.grabs[g_x11hk.n_grabs++];
    g->keycode    = kc;
    g->modifiers  = mods;
    g->action     = action;
    return 1;
}

void snapx_hotkey_init(SnapxConfig *config)
{
    if (!config) return;

    g_x11hk.dpy = XOpenDisplay(NULL);
    if (!g_x11hk.dpy) {
        fprintf(stderr, "[hotkey] X11 not available, hotkey disabled.\n");
        return;
    }

    g_x11hk.root    = DefaultRootWindow(g_x11hk.dpy);
    g_x11hk.n_grabs = 0;
    g_x11hk.stop    = 0;

    x11_add_grab(g_x11hk.dpy, config->shortcuts.global_capture,
                 SNAPX_HOTKEY_DEFAULT_MODE);
    x11_add_grab(g_x11hk.dpy, config->shortcuts.capture_fullscreen,
                 SNAPX_HOTKEY_CAPTURE_FULLSCREEN);
    x11_add_grab(g_x11hk.dpy, config->shortcuts.capture_monitor,
                 SNAPX_HOTKEY_CAPTURE_MONITOR);
    x11_add_grab(g_x11hk.dpy, config->shortcuts.capture_region,
                 SNAPX_HOTKEY_CAPTURE_REGION);
    x11_add_grab(g_x11hk.dpy, config->shortcuts.capture_window,
                 SNAPX_HOTKEY_CAPTURE_WINDOW);

    if (g_x11hk.n_grabs == 0) {
        XCloseDisplay(g_x11hk.dpy);
        g_x11hk.dpy = NULL;
        return;
    }

    int (*prev_err)(Display *, XErrorEvent *) =
        XSetErrorHandler(x11_grab_error_handler);
    XSelectInput(g_x11hk.dpy, g_x11hk.root, KeyPressMask);

    int registered = 0;
    for (int i = 0; i < g_x11hk.n_grabs; i++) {
        g_grab_failed = 0;
        XGrabKey(g_x11hk.dpy, g_x11hk.grabs[i].keycode,
                 g_x11hk.grabs[i].modifiers, g_x11hk.root,
                 True, GrabModeAsync, GrabModeAsync);
        XSync(g_x11hk.dpy, False);
        if (!g_grab_failed)
            registered++;
        else
            fprintf(stderr, "[hotkey] Could not grab keycode=%d mod=0x%x "
                            "(already grabbed by another app).\n",
                    g_x11hk.grabs[i].keycode, g_x11hk.grabs[i].modifiers);
    }
    XSetErrorHandler(prev_err);

    if (registered == 0) {
        fprintf(stderr, "[hotkey] No global hotkeys registered.\n");
        XCloseDisplay(g_x11hk.dpy);
        g_x11hk.dpy = NULL;
        g_x11hk.n_grabs = 0;
        return;
    }

    pthread_t tid;
    pthread_create(&tid, NULL, x11_hotkey_thread, NULL);
    pthread_detach(tid);
    fprintf(stderr, "[hotkey] Registered %d global capture hotkey(s) on X11.\n",
            registered);
}

void snapx_hotkey_cleanup(void)
{
    g_x11hk.stop = 1;
    if (g_x11hk.dpy) {
        for (int i = 0; i < g_x11hk.n_grabs; i++)
            XUngrabKey(g_x11hk.dpy, g_x11hk.grabs[i].keycode,
                       g_x11hk.grabs[i].modifiers, g_x11hk.root);
        XCloseDisplay(g_x11hk.dpy);
    }
    memset(&g_x11hk, 0, sizeof(g_x11hk));
}

#elif defined(SNAPX_PLATFORM_LINUX)

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

#define SNAPX_HOTKEY_ID_BASE 0xBE00

typedef struct {
    char               spec[SNAPX_SHORTCUT_MAX];
    SnapxHotkeyAction  action;
    unsigned           mods;
    unsigned           vk;
    int                registered;
} WinHotkeyEntry;

static HWND            g_msg_hwnd   = NULL;
static HANDLE          g_hk_thread  = NULL;
static WinHotkeyEntry  g_win_hks[SNAPX_MAX_HOTKEY_GRABS];
static int             g_win_n_hks  = 0;

static unsigned parse_win_modifiers(const char *hotkey)
{
    unsigned mods = 0;
    if (strstr(hotkey, "super") || strstr(hotkey, "win"))       mods |= MOD_WIN;
    if (strstr(hotkey, "ctrl")  || strstr(hotkey, "control"))  mods |= MOD_CONTROL;
    if (strstr(hotkey, "alt")   || strstr(hotkey, "mod1"))     mods |= MOD_ALT;
    if (strstr(hotkey, "shift"))                               mods |= MOD_SHIFT;
    return mods;
}

static unsigned int parse_win_vk(const char *hotkey)
{
    const char *p = strrchr(hotkey, '+');
    if (!p) p = hotkey;
    else    p++;
    if (strlen(p) == 1) {
        char c = (char)toupper((unsigned char)*p);
        if (c >= '0' && c <= '9') return (unsigned int)(c);
        return (unsigned int)c;
    }
    if (strcasecmp(p, "plus") == 0 || strcasecmp(p, "equal") == 0)
        return VK_OEM_PLUS;
    if (strcasecmp(p, "minus") == 0)
        return VK_OEM_MINUS;
    if (strcasecmp(p, "print") == 0 || strcasecmp(p, "printscreen") == 0)
        return VK_SNAPSHOT;
    if (strcasecmp(p, "f12") == 0) return VK_F12;
    return 0;
}

static int win_add_entry(const char *spec, SnapxHotkeyAction action)
{
    if (!spec || !spec[0] || g_win_n_hks >= SNAPX_MAX_HOTKEY_GRABS)
        return 0;
    for (int i = 0; i < g_win_n_hks; i++) {
        if (strcasecmp(g_win_hks[i].spec, spec) == 0)
            return 0;
    }
    WinHotkeyEntry *e = &g_win_hks[g_win_n_hks++];
    snprintf(e->spec, sizeof(e->spec), "%s", spec);
    e->action = action;
    e->mods   = parse_win_modifiers(spec);
    e->vk     = parse_win_vk(spec);
    return (e->vk != 0);
}

static unsigned int __stdcall hotkey_thread(void *arg)
{
    (void)arg;
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = DefWindowProcW;
    wc.lpszClassName = L"SnapxHotkeyWnd";
    RegisterClassExW(&wc);
    g_msg_hwnd = CreateWindowExW(0, L"SnapxHotkeyWnd", NULL, 0,
                                  0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);

    int registered = 0;
    for (int i = 0; i < g_win_n_hks; i++) {
        WinHotkeyEntry *e = &g_win_hks[i];
        if (RegisterHotKey(g_msg_hwnd, SNAPX_HOTKEY_ID_BASE + i, e->mods, e->vk)) {
            e->registered = 1;
            registered++;
        } else {
            fprintf(stderr, "[hotkey] RegisterHotKey failed for '%s': %lu\n",
                    e->spec, GetLastError());
        }
    }
    if (registered == 0) {
        fprintf(stderr, "[hotkey] No global hotkeys registered.\n");
        return 0;
    }
    fprintf(stderr, "[hotkey] Registered %d global capture hotkey(s) (Win32).\n",
            registered);

    MSG msg;
    while (GetMessageW(&msg, g_msg_hwnd, 0, 0)) {
        if (msg.message == WM_HOTKEY) {
            int id = (int)msg.wParam - SNAPX_HOTKEY_ID_BASE;
            if (id >= 0 && id < g_win_n_hks && g_win_hks[id].registered && g_hotkey_cb)
                g_hotkey_cb(g_win_hks[id].action, g_hotkey_user_data);
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

void snapx_hotkey_init(SnapxConfig *config)
{
    if (!config) return;
    g_win_n_hks = 0;

    win_add_entry(config->shortcuts.global_capture, SNAPX_HOTKEY_DEFAULT_MODE);
    win_add_entry(config->shortcuts.capture_fullscreen, SNAPX_HOTKEY_CAPTURE_FULLSCREEN);
    win_add_entry(config->shortcuts.capture_monitor, SNAPX_HOTKEY_CAPTURE_MONITOR);
    win_add_entry(config->shortcuts.capture_region, SNAPX_HOTKEY_CAPTURE_REGION);
    win_add_entry(config->shortcuts.capture_window, SNAPX_HOTKEY_CAPTURE_WINDOW);

    if (g_win_n_hks == 0) return;

    g_hk_thread = (HANDLE)_beginthreadex(NULL, 0, hotkey_thread, NULL, 0, NULL);
}

void snapx_hotkey_cleanup(void)
{
    if (g_msg_hwnd) {
        for (int i = 0; i < g_win_n_hks; i++) {
            if (g_win_hks[i].registered)
                UnregisterHotKey(g_msg_hwnd, SNAPX_HOTKEY_ID_BASE + i);
        }
        PostMessageW(g_msg_hwnd, WM_QUIT, 0, 0);
    }
    if (g_hk_thread) {
        WaitForSingleObject(g_hk_thread, 2000);
        CloseHandle(g_hk_thread);
        g_hk_thread = NULL;
    }
    g_msg_hwnd = NULL;
    g_win_n_hks = 0;
}

/* ─────────────────────────────────── macOS ──────────────────────────────── */
#elif defined(SNAPX_PLATFORM_MACOS)

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ApplicationServices/ApplicationServices.h>
#include <pthread.h>

typedef struct {
    CGKeyCode      keycode;
    CGEventFlags   flags;
    SnapxHotkeyAction action;
    char           spec[SNAPX_SHORTCUT_MAX];
} MacHotkeyEntry;

static CFMachPortRef       g_event_tap   = NULL;
static CFRunLoopSourceRef  g_rl_source   = NULL;
static pthread_t           g_tap_thread  = 0;
static MacHotkeyEntry      g_mac_hks[SNAPX_MAX_HOTKEY_GRABS];
static int                 g_mac_n_hks   = 0;

static CGEventFlags parse_mac_flags(const char *hotkey)
{
    CGEventFlags flags = 0;
    if (strstr(hotkey, "ctrl") || strstr(hotkey, "control"))
        flags |= kCGEventFlagMaskControl;
    if (strstr(hotkey, "shift"))
        flags |= kCGEventFlagMaskShift;
    if (strstr(hotkey, "alt") || strstr(hotkey, "mod1"))
        flags |= kCGEventFlagMaskAlternate;
    if (strstr(hotkey, "super") || strstr(hotkey, "cmd") || strstr(hotkey, "meta"))
        flags |= kCGEventFlagMaskCommand;
    return flags;
}

static CGKeyCode parse_mac_keycode(const char *hotkey)
{
    const char *p = strrchr(hotkey, '+');
    if (!p) p = hotkey;
    else    p++;
    if (strlen(p) == 1) {
        char c = (char)tolower((unsigned char)*p);
        if (c >= 'a' && c <= 'z') return (CGKeyCode)(c - 'a' + 0);
        if (c >= '0' && c <= '9') return (CGKeyCode)(c - '0' + 29);
    }
    return 0;
}

static int mac_add_entry(const char *spec, SnapxHotkeyAction action)
{
    if (!spec || !spec[0] || g_mac_n_hks >= SNAPX_MAX_HOTKEY_GRABS)
        return 0;
    for (int i = 0; i < g_mac_n_hks; i++) {
        if (strcasecmp(g_mac_hks[i].spec, spec) == 0)
            return 0;
    }
    MacHotkeyEntry *e = &g_mac_hks[g_mac_n_hks++];
    snprintf(e->spec, sizeof(e->spec), "%s", spec);
    e->action  = action;
    e->flags   = parse_mac_flags(spec);
    e->keycode = parse_mac_keycode(spec);
    return 1;
}

static CGEventRef hotkey_tap_cb(CGEventTapProxy proxy, CGEventType type,
                                  CGEventRef event, void *refcon)
{
    (void)proxy; (void)refcon;
    if (type == kCGEventKeyDown) {
        CGKeyCode kc = (CGKeyCode)CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
        CGEventFlags flags = CGEventGetFlags(event) &
                             (kCGEventFlagMaskCommand | kCGEventFlagMaskShift |
                              kCGEventFlagMaskAlternate | kCGEventFlagMaskControl);
        for (int i = 0; i < g_mac_n_hks; i++) {
            if (kc == g_mac_hks[i].keycode && flags == g_mac_hks[i].flags) {
                if (g_hotkey_cb)
                    g_hotkey_cb(g_mac_hks[i].action, g_hotkey_user_data);
                break;
            }
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
    if (!config) return;
    g_mac_n_hks = 0;

    mac_add_entry(config->shortcuts.global_capture, SNAPX_HOTKEY_DEFAULT_MODE);
    mac_add_entry(config->shortcuts.capture_fullscreen, SNAPX_HOTKEY_CAPTURE_FULLSCREEN);
    mac_add_entry(config->shortcuts.capture_monitor, SNAPX_HOTKEY_CAPTURE_MONITOR);
    mac_add_entry(config->shortcuts.capture_region, SNAPX_HOTKEY_CAPTURE_REGION);
    mac_add_entry(config->shortcuts.capture_window, SNAPX_HOTKEY_CAPTURE_WINDOW);

    if (g_mac_n_hks == 0) return;

    if (!AXIsProcessTrusted()) {
        fprintf(stderr, "[hotkey] Accessibility permission not granted. "
                        "Global hotkey disabled.\n");
        g_mac_n_hks = 0;
        return;
    }

    g_event_tap = CGEventTapCreate(
        kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
        CGEventMaskBit(kCGEventKeyDown), hotkey_tap_cb, NULL);

    if (!g_event_tap) {
        fprintf(stderr, "[hotkey] CGEventTapCreate failed.\n");
        g_mac_n_hks = 0;
        return;
    }

    g_rl_source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, g_event_tap, 0);
    pthread_create(&g_tap_thread, NULL, tap_run_loop, NULL);
    pthread_detach(g_tap_thread);
    fprintf(stderr, "[hotkey] Registered %d global capture hotkey(s) (macOS).\n",
            g_mac_n_hks);
}

void snapx_hotkey_cleanup(void)
{
    if (g_event_tap) {
        CGEventTapEnable(g_event_tap, FALSE);
        CFRelease(g_event_tap);
        g_event_tap = NULL;
    }
    if (g_rl_source) {
        CFRelease(g_rl_source);
        g_rl_source = NULL;
    }
    g_mac_n_hks = 0;
}

#else

void snapx_hotkey_init(SnapxConfig *config) { (void)config; }
void snapx_hotkey_cleanup(void) {}

#endif
