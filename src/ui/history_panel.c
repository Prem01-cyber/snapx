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
    GtkWidget           *empty_label;
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

static GtkWidget *history_row_new(const char *path)
{
    GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(row_box, "snapx-history-row");
    gtk_widget_set_margin_start(row_box, 8);
    gtk_widget_set_margin_end(row_box, 8);
    gtk_widget_set_margin_top(row_box, 6);
    gtk_widget_set_margin_bottom(row_box, 6);

    GtkWidget *thumb;
#ifdef SNAPX_USE_GTK4
    thumb = gtk_picture_new_for_filename(path);
    if (!thumb) {
        thumb = gtk_label_new("?");
        gtk_widget_set_size_request(thumb, 72, 72);
    } else {
        gtk_picture_set_can_shrink(GTK_PICTURE(thumb), TRUE);
        gtk_picture_set_content_fit(GTK_PICTURE(thumb), GTK_CONTENT_FIT_COVER);
        gtk_widget_set_size_request(thumb, 72, 72);
        gtk_widget_add_css_class(thumb, "snapx-history-thumb");
    }
#else
    GError *err = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path, 72, 72, TRUE, &err);
    if (pb) {
        thumb = gtk_image_new_from_pixbuf(pb);
        g_object_unref(pb);
        gtk_widget_set_size_request(thumb, 72, 72);
        gtk_widget_add_css_class(thumb, "snapx-history-thumb");
    } else {
        if (err) g_error_free(err);
        thumb = gtk_label_new("?");
        gtk_widget_set_size_request(thumb, 72, 72);
    }
#endif

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    GtkWidget *lbl = gtk_label_new(base);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.5f);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(lbl, "snapx-history-name");

    gtk_box_append(GTK_BOX(row_box), thumb);
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

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(outer, "snapx-history");

    GtkWidget *title = gtk_label_new("Recent");
    gtk_widget_add_css_class(title, "snapx-settings-section");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_widget_set_margin_start(title, 10);
    gtk_widget_set_margin_top(title, 8);
    gtk_widget_set_margin_bottom(title, 4);
    gtk_box_append(GTK_BOX(outer), title);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);

    hp->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(hp->list), GTK_SELECTION_NONE);
    g_signal_connect(hp->list, "row-activated", G_CALLBACK(on_row_activated), hp);

    hp->empty_label = gtk_label_new("No recent saves");
    gtk_widget_add_css_class(hp->empty_label, "dim-label");
    gtk_widget_set_halign(hp->empty_label, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(hp->empty_label, 24);
    gtk_widget_set_visible(hp->empty_label, FALSE);

    GtkWidget *list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(list_box), hp->list);
    gtk_box_append(GTK_BOX(list_box), hp->empty_label);

#ifdef SNAPX_USE_GTK4
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list_box);
#else
    gtk_container_add(GTK_CONTAINER(scroll), list_box);
#endif
    gtk_box_append(GTK_BOX(outer), scroll);

    g_object_set_data(G_OBJECT(outer), "snapx-history-panel", hp);
    snapx_history_panel_refresh(outer, config);
    return outer;
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
