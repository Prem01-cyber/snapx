/**
 * @file history_panel.c
 * @brief Overlay file-list of recent saves in save_dir.
 */

#include "history_panel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#define HISTORY_PANEL_WIDTH 220

typedef struct {
    SnapxHistorySelectFn on_select;
    gpointer             userdata;
    GtkWidget           *outer;
    GtkWidget           *list;
    GtkWidget           *empty_label;
    char                *paths[64];
    int                  n_paths;
    gboolean             open;
} HistoryPanel;

typedef struct { char path[1024]; time_t mtime; } HistItem;

static int is_image_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    return strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".jpg") == 0
        || strcasecmp(dot, ".jpeg") == 0 || strcasecmp(dot, ".webp") == 0;
}

static void history_clear_paths(HistoryPanel *hp)
{
    for (int i = 0; i < hp->n_paths; i++)
        g_free(hp->paths[i]);
    hp->n_paths = 0;
}

static void history_panel_apply_open(HistoryPanel *hp)
{
    if (!hp || !hp->outer) return;
    gtk_widget_set_visible(hp->outer, hp->open);
    if (hp->open)
        gtk_widget_add_css_class(hp->outer, "open");
    else
        gtk_widget_remove_css_class(hp->outer, "open");
}

static void on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
    (void)box;
    HistoryPanel *hp = data;
    int idx = gtk_list_box_row_get_index(row);
    if (idx < 0 || idx >= hp->n_paths) return;
    if (hp->on_select)
        hp->on_select(hp->paths[idx], hp->userdata);
}

static GtkWidget *history_row_new(const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(row_box, "snapx-history-row");
    gtk_widget_set_margin_start(row_box, 8);
    gtk_widget_set_margin_end(row_box, 8);
    gtk_widget_set_margin_top(row_box, 2);
    gtk_widget_set_margin_bottom(row_box, 2);

    GtkWidget *lbl = gtk_label_new(base);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_add_css_class(lbl, "snapx-history-name");
    gtk_box_append(GTK_BOX(row_box), lbl);
    return row_box;
}

GtkWidget *snapx_history_panel_new(const SnapxConfig *config,
                                      SnapxHistorySelectFn on_select,
                                      gpointer userdata)
{
    HistoryPanel *hp = g_malloc0(sizeof(*hp));
    hp->on_select = on_select;
    hp->userdata  = userdata;
    hp->open      = FALSE;

    hp->outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(hp->outer, "snapx-history");
    gtk_widget_set_halign(hp->outer, GTK_ALIGN_START);
    gtk_widget_set_valign(hp->outer, GTK_ALIGN_FILL);
    gtk_widget_set_size_request(hp->outer, HISTORY_PANEL_WIDTH, -1);
    gtk_widget_set_can_focus(hp->outer, FALSE);
    gtk_widget_set_visible(hp->outer, FALSE);

    GtkWidget *title = gtk_label_new("Recent saves");
    gtk_widget_add_css_class(title, "snapx-history-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_widget_set_margin_start(title, 10);
    gtk_widget_set_margin_end(title, 10);
    gtk_widget_set_margin_top(title, 8);
    gtk_widget_set_margin_bottom(title, 4);
    gtk_box_append(GTK_BOX(hp->outer), title);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);

    hp->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(hp->list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(hp->list, "snapx-history-list");
    g_signal_connect(hp->list, "row-activated", G_CALLBACK(on_row_activated), hp);

    hp->empty_label = gtk_label_new("No recent saves");
    gtk_widget_add_css_class(hp->empty_label, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(hp->empty_label), TRUE);
    gtk_widget_set_halign(hp->empty_label, GTK_ALIGN_START);
    gtk_widget_set_margin_start(hp->empty_label, 10);
    gtk_widget_set_margin_end(hp->empty_label, 10);
    gtk_widget_set_margin_top(hp->empty_label, 8);
    gtk_widget_set_visible(hp->empty_label, FALSE);

    GtkWidget *list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(list_box), hp->list);
    gtk_box_append(GTK_BOX(list_box), hp->empty_label);

#ifdef SNAPX_USE_GTK4
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list_box);
#else
    gtk_container_add(GTK_CONTAINER(scroll), list_box);
#endif
    gtk_box_append(GTK_BOX(hp->outer), scroll);

    g_object_set_data(G_OBJECT(hp->outer), "snapx-history-panel", hp);
    snapx_history_panel_refresh(hp->outer, config);
    return hp->outer;
}

void snapx_history_panel_set_open(GtkWidget *panel, gboolean open)
{
    HistoryPanel *hp = g_object_get_data(G_OBJECT(panel), "snapx-history-panel");
    if (!hp) return;
    hp->open = open;
    history_panel_apply_open(hp);
}

gboolean snapx_history_panel_is_open(GtkWidget *panel)
{
    HistoryPanel *hp = g_object_get_data(G_OBJECT(panel), "snapx-history-panel");
    return hp ? hp->open : FALSE;
}

void snapx_history_panel_toggle(GtkWidget *panel)
{
    snapx_history_panel_set_open(panel, !snapx_history_panel_is_open(panel));
}

void snapx_history_panel_refresh(GtkWidget *panel, const SnapxConfig *config)
{
    HistoryPanel *hp = g_object_get_data(G_OBJECT(panel), "snapx-history-panel");
    if (!hp || !hp->list || !config) return;

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(hp->list)) != NULL)
        gtk_list_box_remove(GTK_LIST_BOX(hp->list), child);
    history_clear_paths(hp);

    DIR *d = opendir(config->save_dir);
    if (!d) {
        gtk_widget_set_visible(hp->empty_label, TRUE);
        gtk_label_set_text(GTK_LABEL(hp->empty_label), "Save folder not found");
        return;
    }

    HistItem items[64];
    int n = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && n < 64) {
        if (!is_image_ext(ent->d_name)) continue;
        char full[1024];
        int dir_len = (int)strlen(config->save_dir);
        int name_len = (int)strlen(ent->d_name);
        if (dir_len + 1 + name_len >= (int)sizeof(full))
            continue;
        snprintf(full, sizeof(full), "%s/%s", config->save_dir, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (strlen(full) >= sizeof(items[n].path))
            continue;
        snprintf(items[n].path, sizeof(items[n].path), "%s", full);
        items[n].mtime = st.st_mtime;
        n++;
    }
    closedir(d);

    if (n == 0) {
        gtk_widget_set_visible(hp->empty_label, TRUE);
        char msg[576];
        snprintf(msg, sizeof(msg), "No recent saves in\n%s", config->save_dir);
        gtk_label_set_text(GTK_LABEL(hp->empty_label), msg);
        return;
    }

    gtk_widget_set_visible(hp->empty_label, FALSE);

    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (items[j].mtime > items[i].mtime) {
                HistItem tmp = items[i];
                items[i] = items[j];
                items[j] = tmp;
            }

    int show = n < 20 ? n : 20;
    for (int i = 0; i < show; i++) {
        hp->paths[hp->n_paths] = g_strdup(items[i].path);
        GtkWidget *row_w = history_row_new(items[i].path);
        gtk_list_box_append(GTK_LIST_BOX(hp->list), row_w);
        hp->n_paths++;
    }
}
