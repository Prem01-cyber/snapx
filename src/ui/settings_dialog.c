/**
 * @file settings_dialog.c
 * @brief Settings dialog — GtkWindow + GtkDropDown (GTK4 modern API, no deprecated widgets).
 */

#include "settings_dialog.h"
#include "../utils/config.h"

#include <string.h>
#include <stdio.h>

/* ─── Labelled grid row helper ───────────────────────────────────────────── */

static GtkWidget *make_row(GtkWidget *grid, int row,
                            const char *label_text, GtkWidget *control)
{
    GtkWidget *lbl = gtk_label_new(label_text);
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    gtk_widget_set_margin_end(lbl, 8);
    gtk_widget_set_margin_bottom(lbl, 6);
    gtk_grid_attach(GTK_GRID(grid), lbl,     0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), control, 1, row, 1, 1);
    gtk_widget_set_hexpand(control, TRUE);
    gtk_widget_set_margin_bottom(control, 6);
    return control;
}

/* ─── Shared dialog widgets referenced by OK callback ───────────────────── */

typedef struct {
    SnapxConfig *config;
    GtkWidget   *win;
    GtkWidget   *dir_entry;
    GtkWidget   *pattern_entry;
    GtkWidget   *mode_dd;
    GtkWidget   *delay_spin;
    GtkWidget   *cursor_sw;
    GtkWidget   *fmt_dd;
    GtkWidget   *quality_spin;
    GtkWidget   *clipboard_sw;
    GtkWidget   *sound_sw;
    GMainLoop   *loop;
} DlgWidgets;

static void on_ok_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    DlgWidgets *dw = (DlgWidgets *)user_data;
    SnapxConfig *c = dw->config;

    snprintf(c->save_dir, sizeof(c->save_dir), "%s",
             gtk_editable_get_text(GTK_EDITABLE(dw->dir_entry)));
    snprintf(c->filename_pattern, sizeof(c->filename_pattern), "%s",
             gtk_editable_get_text(GTK_EDITABLE(dw->pattern_entry)));

    c->default_mode   = (SnapxCaptureMode)
        gtk_drop_down_get_selected(GTK_DROP_DOWN(dw->mode_dd));
    c->default_delay  = (int)gtk_spin_button_get_value(
        GTK_SPIN_BUTTON(dw->delay_spin));
    c->show_cursor    = gtk_switch_get_active(GTK_SWITCH(dw->cursor_sw));

    c->default_format = (SnapxOutputFormat)
        gtk_drop_down_get_selected(GTK_DROP_DOWN(dw->fmt_dd));
    c->jpeg_quality   = (int)gtk_spin_button_get_value(
        GTK_SPIN_BUTTON(dw->quality_spin));
    c->auto_clipboard = gtk_switch_get_active(GTK_SWITCH(dw->clipboard_sw));
    c->play_sound     = gtk_switch_get_active(GTK_SWITCH(dw->sound_sw));

    snapx_config_save(c);
    gtk_window_close(GTK_WINDOW(dw->win));
}

static gboolean on_close_request(GtkWindow *win, gpointer user_data)
{
    (void)win;
    GMainLoop *loop = (GMainLoop *)user_data;
    g_main_loop_quit(loop);
    return FALSE; /* allow close */
}

/* ─── Live pattern preview ───────────────────────────────────────────────── */

typedef struct {
    GtkWidget   *pattern_entry;
    GtkWidget   *preview_label;
    SnapxConfig *config;
} PatternPreviewCtx;

static void update_preview(PatternPreviewCtx *ctx)
{
    const char *p = gtk_editable_get_text(GTK_EDITABLE(ctx->pattern_entry));
    snprintf(ctx->config->filename_pattern,
             sizeof(ctx->config->filename_pattern), "%s", p);
    char tmp[512];
    snapx_config_build_path(ctx->config, tmp, sizeof(tmp));
    char markup[640];
    snprintf(markup, sizeof(markup), "<small><i>%s</i></small>", tmp);
    gtk_label_set_markup(GTK_LABEL(ctx->preview_label), markup);
}

static void on_pattern_changed(GtkEditable *e, gpointer data)
{
    (void)e;
    update_preview((PatternPreviewCtx *)data);
}

/* ─── Public API ─────────────────────────────────────────────────────────── */

void snapx_settings_dialog_show(GtkWindow *parent, SnapxConfig *config)
{
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);

    /* Root window */
    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), "snapx Preferences");
    gtk_window_set_default_size(GTK_WINDOW(win), 540, -1);
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    if (parent)
        gtk_window_set_transient_for(GTK_WINDOW(win), parent);

    /* Header bar */
    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), FALSE);
    gtk_window_set_titlebar(GTK_WINDOW(win), header);

    GtkWidget *btn_cancel = gtk_button_new_with_label("Cancel");
    GtkWidget *btn_ok     = gtk_button_new_with_label("OK");
    gtk_widget_add_css_class(btn_ok, "suggested-action");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), btn_cancel);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), btn_ok);
    g_signal_connect_swapped(btn_cancel, "clicked",
                              G_CALLBACK(gtk_window_close), win);

    /* Main content */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);
    gtk_window_set_child(GTK_WINDOW(win), vbox);

    GtkWidget *notebook = gtk_notebook_new();
    gtk_box_append(GTK_BOX(vbox), notebook);

    /* ── Tab 1: Save ───────────────────────────────────────────────────── */
    GtkWidget *save_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(save_grid), 8);
    gtk_widget_set_margin_top(save_grid, 12);
    gtk_widget_set_margin_start(save_grid, 8);
    gtk_widget_set_margin_end(save_grid, 8);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), save_grid,
                              gtk_label_new("Save"));
    int r = 0;

    GtkWidget *dir_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(dir_entry), config->save_dir);
    make_row(save_grid, r++, "Save directory:", dir_entry);

    GtkWidget *pattern_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(pattern_entry), config->filename_pattern);
    make_row(save_grid, r++, "Filename pattern:", pattern_entry);

    GtkWidget *preview_lbl = gtk_label_new("");
    gtk_widget_set_halign(preview_lbl, GTK_ALIGN_START);
    PatternPreviewCtx *pctx = g_new(PatternPreviewCtx, 1);
    pctx->pattern_entry = pattern_entry;
    pctx->preview_label = preview_lbl;
    pctx->config        = config;
    update_preview(pctx);
    g_signal_connect(pattern_entry, "changed", G_CALLBACK(on_pattern_changed), pctx);
    make_row(save_grid, r++, "Preview:", preview_lbl);

    /* ── Tab 2: Capture ────────────────────────────────────────────────── */
    GtkWidget *cap_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(cap_grid), 8);
    gtk_widget_set_margin_top(cap_grid, 12);
    gtk_widget_set_margin_start(cap_grid, 8);
    gtk_widget_set_margin_end(cap_grid, 8);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), cap_grid,
                              gtk_label_new("Capture"));
    r = 0;

    const char *modes[] = { "Full Screen", "Single Monitor", "Region",
                             "Window", "Active Window", NULL };
    GtkWidget *mode_dd = gtk_drop_down_new_from_strings(modes);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(mode_dd), (guint)config->default_mode);
    make_row(cap_grid, r++, "Default mode:", mode_dd);

    GtkWidget *delay_spin = gtk_spin_button_new_with_range(0, 60, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(delay_spin),
                               (double)config->default_delay);
    make_row(cap_grid, r++, "Default delay (s):", delay_spin);

    GtkWidget *cursor_sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(cursor_sw), config->show_cursor);
    make_row(cap_grid, r++, "Include cursor:", cursor_sw);

    /* ── Tab 3: Output ─────────────────────────────────────────────────── */
    GtkWidget *out_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(out_grid), 8);
    gtk_widget_set_margin_top(out_grid, 12);
    gtk_widget_set_margin_start(out_grid, 8);
    gtk_widget_set_margin_end(out_grid, 8);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), out_grid,
                              gtk_label_new("Output"));
    r = 0;

    const char *fmts[] = { "PNG", "JPEG", "WebP", NULL };
    GtkWidget *fmt_dd = gtk_drop_down_new_from_strings(fmts);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(fmt_dd),
                                (guint)config->default_format);
    make_row(out_grid, r++, "Default format:", fmt_dd);

    GtkWidget *quality_spin = gtk_spin_button_new_with_range(1, 100, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(quality_spin),
                               (double)config->jpeg_quality);
    make_row(out_grid, r++, "JPEG quality:", quality_spin);

    GtkWidget *clipboard_sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(clipboard_sw), config->auto_clipboard);
    make_row(out_grid, r++, "Auto copy to clipboard:", clipboard_sw);

    GtkWidget *sound_sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sound_sw), config->play_sound);
    make_row(out_grid, r++, "Play sound on capture:", sound_sw);

    /* ── Wire OK button ─────────────────────────────────────────────────── */
    DlgWidgets *dw = g_new(DlgWidgets, 1);
    dw->config        = config;
    dw->win           = win;
    dw->dir_entry     = dir_entry;
    dw->pattern_entry = pattern_entry;
    dw->mode_dd       = mode_dd;
    dw->delay_spin    = delay_spin;
    dw->cursor_sw     = cursor_sw;
    dw->fmt_dd        = fmt_dd;
    dw->quality_spin  = quality_spin;
    dw->clipboard_sw  = clipboard_sw;
    dw->sound_sw      = sound_sw;
    dw->loop          = loop;

    g_signal_connect(btn_ok, "clicked", G_CALLBACK(on_ok_clicked), dw);
    g_signal_connect(win, "close-request", G_CALLBACK(on_close_request), loop);

    gtk_widget_set_visible(win, TRUE);
    g_main_loop_run(loop);

    g_free(pctx);
    g_free(dw);
    g_main_loop_unref(loop);
}
