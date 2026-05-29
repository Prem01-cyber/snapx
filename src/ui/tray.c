/**
 * @file tray.c
 * @brief Background tray via GApplication actions.
 */

#include "tray.h"
#include "window_main.h"

#include <stdio.h>

static int g_background;
static GtkApplication *g_app;

static void action_quit(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a; (void)p; (void)d;
    if (g_app) g_application_quit(G_APPLICATION(g_app));
}

static void action_show(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a; (void)p; (void)d;
    snapx_window_main_show();
}

static void action_capture_region(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a; (void)p; (void)d;
    snapx_window_main_capture_region();
}

void snapx_tray_init(void *mw_opaque, GtkApplication *app, SnapxConfig *config)
{
    (void)mw_opaque;
    g_app = app;

    static GActionEntry entries[] = {
        { .name = "quit",           .activate = action_quit },
        { .name = "show",           .activate = action_show },
        { .name = "capture-region", .activate = action_capture_region },
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app), entries,
                                    G_N_ELEMENTS(entries), NULL);

    if (config && (config->start_in_tray || config->close_to_tray))
        g_application_hold(G_APPLICATION(app));

    g_background = config && config->start_in_tray;
    fprintf(stderr, "[tray] Background mode ready\n");
}

void snapx_tray_set_visible(int visible) { g_background = visible; }
int  snapx_tray_is_background(void) { return g_background; }
