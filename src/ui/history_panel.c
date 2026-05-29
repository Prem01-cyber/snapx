/**
 * @file history_panel.c
 * @brief Thumbnail list of recent saves in save_dir.
 */

#include "history_panel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

typedef struct {
    SnapxHistorySelectFn on_select;
    gpointer             userdata;
    GtkWidget           *list;
    char                *paths[64];
    int                  n_paths;
} HistoryPanel;

typedef struct { char path[512]; time_t mtime; } HistItem;

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

static void on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
    (void)box;
    HistoryPanel *hp = data;
    int idx = gtk_list_box_row_get_index(row);
    if (idx < 0 || idx >= hp->n_paths) return;
    if (hp->on_select)
        hp->on_select(hp->paths[idx], hp->userdata);
}

GtkWidget *snapx_history_panel_new(const SnapxConfig *config,
                                      SnapxHistorySelectFn on_select,
                                      gpointer userdata)
{
    HistoryPanel *hp = g_malloc0(sizeof(*hp));
    hp->on_select = on_select;
    hp->userdata  = userdata;

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *title = gtk_label_new("Recent");
    gtk_widget_add_css_class(title, "snapx-settings-section");
    gtk_box_append(GTK_BOX(box), title);

    hp->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(hp->list), GTK_SELECTION_NONE);
    gtk_widget_set_vexpand(hp->list, TRUE);
    g_signal_connect(hp->list, "row-activated", G_CALLBACK(on_row_activated), hp);
    gtk_box_append(GTK_BOX(box), hp->list);

    g_object_set_data(G_OBJECT(box), "snapx-history-panel", hp);
    snapx_history_panel_refresh(box, config);
    return box;
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
    if (!d) return;

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

    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (items[j].mtime > items[i].mtime) {
                HistItem tmp = items[i];
                items[i] = items[j];
                items[j] = tmp;
            }

    int show = n < 20 ? n : 20;
    for (int i = 0; i < show; i++) {
        const char *base = strrchr(items[i].path, '/');
        hp->paths[hp->n_paths] = g_strdup(items[i].path);
        GtkWidget *lbl = gtk_label_new(base ? base + 1 : items[i].path);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_list_box_append(GTK_LIST_BOX(hp->list), lbl);
        hp->n_paths++;
    }
}
