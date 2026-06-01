/**
 * @file settings_dialog.c
 * @brief Settings dialog — preferences including filename patterns and shortcuts.
 */

#include "settings_dialog.h"
#include "../utils/config.h"
#include "../utils/shortcut.h"
#include "../output/upload.h"
#include "../output/ocr.h"

#include <string.h>
#include <stdio.h>

static GtkWidget *g_recording_entry = NULL;

/* ─── Layout helpers ─────────────────────────────────────────────────────── */

static void set_section_title(GtkWidget *lbl, const char *title)
{
    char *escaped = g_markup_escape_text(title, -1);
    char *markup = g_strdup_printf("<b>%s</b>", escaped);
    gtk_label_set_markup(GTK_LABEL(lbl), markup);
    g_free(markup);
    g_free(escaped);
}

static GtkWidget *wrap_tab_scroll(GtkWidget *content)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), content);
    return scroll;
}

static GtkWidget *new_tab_page(void)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_start(page, 20);
    gtk_widget_set_margin_end(page, 20);
    gtk_widget_set_margin_top(page, 16);
    gtk_widget_set_margin_bottom(page, 20);
    gtk_widget_add_css_class(page, "snapx-pref-page");
    return page;
}

static GtkWidget *new_pref_group(const char *title)
{
    GtkWidget *group = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(group, "snapx-pref-group");

    if (title && title[0]) {
        GtkWidget *lbl = gtk_label_new(NULL);
        set_section_title(lbl, title);
        gtk_widget_add_css_class(lbl, "snapx-settings-section");
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(group), lbl);
    }

    GtkWidget *rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(rows, "snapx-pref-rows");
    gtk_box_append(GTK_BOX(group), rows);
    return rows;
}

static GtkWidget *make_pref_row(const char *label_text, GtkWidget *control)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(row, "snapx-pref-row");

    GtkWidget *lbl = gtk_label_new(label_text);
    gtk_widget_add_css_class(lbl, "snapx-pref-label");
    gtk_label_set_xalign(GTK_LABEL(lbl), 1.0f);
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), lbl);

    gtk_widget_set_hexpand(control, TRUE);
    gtk_widget_set_halign(control, GTK_ALIGN_FILL);
    gtk_box_append(GTK_BOX(row), control);
    return row;
}

/** Row for toggles: label on the left, switch anchored on the right. */
static GtkWidget *make_switch_row(const char *label_text, GtkWidget *sw)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(row, "snapx-switch-row");

    GtkWidget *lbl = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_box_append(GTK_BOX(row), lbl);

    gtk_widget_set_hexpand(sw, FALSE);
    gtk_widget_set_vexpand(sw, FALSE);
    gtk_widget_set_halign(sw, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(sw, 52, 26);

    GtkWidget *tail = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(tail, "snapx-switch-tail");
    gtk_widget_set_hexpand(tail, FALSE);
    gtk_widget_set_halign(tail, GTK_ALIGN_END);
    gtk_widget_set_valign(tail, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(tail), sw);
    gtk_box_append(GTK_BOX(row), tail);
    return row;
}

/** Row for compact controls (spin, dropdown, color): label column + left-aligned widget. */
static GtkWidget *make_pref_row_compact(const char *label_text, GtkWidget *control)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(row, "snapx-pref-row");

    GtkWidget *lbl = gtk_label_new(label_text);
    gtk_widget_add_css_class(lbl, "snapx-pref-label");
    gtk_label_set_xalign(GTK_LABEL(lbl), 1.0f);
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), lbl);

    gtk_widget_set_hexpand(control, FALSE);
    gtk_widget_set_halign(control, GTK_ALIGN_START);
    gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), control);
    return row;
}

static void pref_row_add(GtkWidget *rows, const char *label, GtkWidget *control)
{
    gtk_box_append(GTK_BOX(rows), make_pref_row(label, control));
}

static void pref_row_add_compact(GtkWidget *rows, const char *label, GtkWidget *control)
{
    gtk_box_append(GTK_BOX(rows), make_pref_row_compact(label, control));
}

static void pref_row_add_switch(GtkWidget *rows, const char *label, GtkWidget *sw)
{
    gtk_box_append(GTK_BOX(rows), make_switch_row(label, sw));
}

static void on_record_clicked(GtkButton *btn, gpointer entry);
static GtkWidget *new_rgb_button(double r, double g, double b, const char *title);
static void read_rgb_button(GtkWidget *btn, double *r, double *g, double *b);

static void on_clear_shortcut(GtkButton *btn, gpointer entry)
{
    (void)btn;
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    if (g_recording_entry == entry)
        g_recording_entry = NULL;
    gtk_widget_remove_css_class(GTK_WIDGET(entry), "snapx-recording");
}

static GtkWidget *new_record_button(GtkWidget *entry)
{
    GtkWidget *b = gtk_button_new_with_label("Record");
    gtk_widget_add_css_class(b, "snapx-shortcut-btn");
    g_signal_connect(b, "clicked", G_CALLBACK(on_record_clicked), entry);
    return b;
}

static GtkWidget *new_clear_button(GtkWidget *entry)
{
    GtkWidget *b = gtk_button_new_with_label("Clear");
    gtk_widget_add_css_class(b, "snapx-shortcut-btn");
    g_signal_connect(b, "clicked", G_CALLBACK(on_clear_shortcut), entry);
    return b;
}

static GtkWidget *new_shortcuts_grid(void)
{
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_widget_add_css_class(grid, "snapx-shortcuts-grid");
    return grid;
}

static void shortcuts_grid_add_header(GtkWidget *grid, int row)
{
    GtkWidget *h_action = gtk_label_new("Action");
    GtkWidget *h_key    = gtk_label_new("Shortcut");
    GtkWidget *h_record = gtk_label_new("Record");
    GtkWidget *h_clear  = gtk_label_new("Clear");
    gtk_widget_add_css_class(h_action, "snapx-shortcuts-colhead");
    gtk_widget_add_css_class(h_key, "snapx-shortcuts-colhead");
    gtk_widget_add_css_class(h_record, "snapx-shortcuts-colhead");
    gtk_widget_add_css_class(h_clear, "snapx-shortcuts-colhead");
    gtk_label_set_xalign(GTK_LABEL(h_action), 0.0f);
    gtk_label_set_xalign(GTK_LABEL(h_key), 0.0f);
    gtk_label_set_xalign(GTK_LABEL(h_record), 0.5f);
    gtk_label_set_xalign(GTK_LABEL(h_clear), 0.5f);
    gtk_widget_set_halign(h_record, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(h_clear, GTK_ALIGN_CENTER);
    gtk_grid_attach(GTK_GRID(grid), h_action, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), h_key,    1, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), h_record, 2, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), h_clear,  3, row, 1, 1);
}

static GtkWidget *shortcuts_grid_add(GtkWidget *grid, int row,
                                      const char *name, GtkWidget *entry)
{
    GtkWidget *lbl = gtk_label_new(name);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(lbl, "snapx-shortcuts-name");

    GtkWidget *record = new_record_button(entry);
    GtkWidget *clear  = new_clear_button(entry);

    gtk_widget_set_hexpand(entry, TRUE);
    gtk_widget_set_halign(entry, GTK_ALIGN_FILL);

    gtk_grid_attach(GTK_GRID(grid), lbl,    0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), entry,  1, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), record, 2, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), clear,  3, row, 1, 1);
    return entry;
}

static void shortcuts_section_add(GtkWidget *page, const char *title,
                                   GtkWidget *grid, gboolean first)
{
    if (title && title[0]) {
        GtkWidget *lbl = gtk_label_new(NULL);
        set_section_title(lbl, title);
        gtk_widget_add_css_class(lbl, "snapx-settings-section");
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        if (!first)
            gtk_widget_set_margin_top(lbl, 12);
        gtk_box_append(GTK_BOX(page), lbl);
    }
    gtk_box_append(GTK_BOX(page), grid);
}

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
    GtkWidget   *upload_svc_dd;
    GtkWidget   *imgur_entry;
    GtkWidget   *custom_url_entry;
    GtkWidget   *copy_url_sw;
    GtkWidget   *auto_upload_sw;
    GtkWidget   *tool_dd;
    GtkWidget   *color_btn;
    /* Beautiful export */
    GtkWidget   *bf_enable_sw;
    GtkWidget   *bf_padding_spin;
    GtkWidget   *bf_bg_dd;
    GtkWidget   *bf_color1;
    GtkWidget   *bf_color2;
    GtkWidget   *bf_corner_spin;
    GtkWidget   *bf_shadow_sw;
    GtkWidget   *bf_shadow_spin;
    GtkWidget   *sc_global;
    GtkWidget   *sc_capture_full;
    GtkWidget   *sc_capture_monitor;
    GtkWidget   *sc_capture_region;
    GtkWidget   *sc_capture_window;
    GtkWidget   *sc_save;
    GtkWidget   *sc_copy;
    GtkWidget   *sc_undo;
    GtkWidget   *sc_redo;
    GtkWidget   *sc_fit;
    GtkWidget   *sc_zoom_in;
    GtkWidget   *sc_zoom_out;
    GtkWidget   *sc_region_ok;
    GtkWidget   *sc_region_cancel;
    GtkWidget   *sc_upload;
    GtkWidget   *sc_ocr;
    GtkWidget   *sc_pin;
    GtkWidget   *sc_crop;
    GtkWidget   *magnifier_sw;
    GtkWidget   *magnifier_zoom_spin;
    GtkWidget   *window_snap_sw;
    GtkWidget   *close_to_tray_sw;
    GtkWidget   *start_in_tray_sw;
    GtkWidget   *ocr_lang_entry;
    GtkWidget   *err_label;
    GMainLoop   *loop;
} DlgWidgets;

static void apply_shortcuts_to_entries(DlgWidgets *dw, const SnapxShortcuts *sc)
{
    gtk_editable_set_text(GTK_EDITABLE(dw->sc_global), sc->global_capture);
    gtk_editable_set_text(GTK_EDITABLE(dw->sc_capture_full), sc->capture_fullscreen);
    gtk_editable_set_text(GTK_EDITABLE(dw->sc_capture_monitor), sc->capture_monitor);
    gtk_editable_set_text(GTK_EDITABLE(dw->sc_capture_region), sc->capture_region);
    gtk_editable_set_text(GTK_EDITABLE(dw->sc_capture_window), sc->capture_window);
    gtk_editable_set_text(GTK_EDITABLE(dw->sc_save), sc->save);
    gtk_editable_set_text(GTK_EDITABLE(dw->sc_copy), sc->copy);
    gtk_editable_set_text(GTK_EDITABLE(dw->sc_undo), sc->undo);
    gtk_editable_set_text(GTK_EDITABLE(dw->sc_redo), sc->redo);
    gtk_editable_set_text(GTK_EDITABLE(dw->sc_fit), sc->fit);
    gtk_editable_set_text(GTK_EDITABLE(dw->sc_zoom_in), sc->zoom_in);
    gtk_editable_set_text(GTK_EDITABLE(dw->sc_zoom_out), sc->zoom_out);
    gtk_editable_set_text(GTK_EDITABLE(dw->sc_region_ok), sc->region_confirm);
    gtk_editable_set_text(GTK_EDITABLE(dw->sc_region_cancel), sc->region_cancel);
    gtk_editable_set_text(GTK_EDITABLE(dw->sc_upload), sc->upload);
    gtk_editable_set_text(GTK_EDITABLE(dw->sc_ocr), sc->ocr);
    gtk_editable_set_text(GTK_EDITABLE(dw->sc_pin), sc->pin);
    gtk_editable_set_text(GTK_EDITABLE(dw->sc_crop), sc->crop);
}

static void on_reset_shortcuts(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    DlgWidgets *dw = user_data;
    SnapxShortcuts defs;
    snapx_shortcuts_set_defaults(&defs);
    apply_shortcuts_to_entries(dw, &defs);
    gtk_label_set_text(GTK_LABEL(dw->err_label), "");
}

static void on_record_clicked(GtkButton *btn, gpointer entry)
{
    (void)btn;
    g_recording_entry = GTK_WIDGET(entry);
    gtk_widget_add_css_class(g_recording_entry, "snapx-recording");
}

static gboolean on_settings_key(GtkEventControllerKey *ctrl, guint keyval,
                                  guint keycode, GdkModifierType state,
                                  gpointer data)
{
    (void)ctrl; (void)keycode; (void)data;
    if (!g_recording_entry) return FALSE;

    if (keyval == GDK_KEY_Escape) {
        gtk_widget_remove_css_class(g_recording_entry, "snapx-recording");
        g_recording_entry = NULL;
        return TRUE;
    }

    char spec[SNAPX_SHORTCUT_MAX];
    snapx_shortcut_from_event(keyval, state, spec, sizeof(spec));
    if (spec[0]) {
        gtk_editable_set_text(GTK_EDITABLE(g_recording_entry), spec);
        gtk_widget_remove_css_class(g_recording_entry, "snapx-recording");
        g_recording_entry = NULL;
        return TRUE;
    }
    return FALSE;
}

static void read_shortcuts_from_ui(DlgWidgets *dw, SnapxShortcuts *sc)
{
    snprintf(sc->global_capture, sizeof(sc->global_capture), "%s",
             gtk_editable_get_text(GTK_EDITABLE(dw->sc_global)));
    snprintf(sc->capture_fullscreen, sizeof(sc->capture_fullscreen), "%s",
             gtk_editable_get_text(GTK_EDITABLE(dw->sc_capture_full)));
    snprintf(sc->capture_monitor, sizeof(sc->capture_monitor), "%s",
             gtk_editable_get_text(GTK_EDITABLE(dw->sc_capture_monitor)));
    snprintf(sc->capture_region, sizeof(sc->capture_region), "%s",
             gtk_editable_get_text(GTK_EDITABLE(dw->sc_capture_region)));
    snprintf(sc->capture_window, sizeof(sc->capture_window), "%s",
             gtk_editable_get_text(GTK_EDITABLE(dw->sc_capture_window)));
    snprintf(sc->save, sizeof(sc->save), "%s",
             gtk_editable_get_text(GTK_EDITABLE(dw->sc_save)));
    snprintf(sc->copy, sizeof(sc->copy), "%s",
             gtk_editable_get_text(GTK_EDITABLE(dw->sc_copy)));
    snprintf(sc->undo, sizeof(sc->undo), "%s",
             gtk_editable_get_text(GTK_EDITABLE(dw->sc_undo)));
    snprintf(sc->redo, sizeof(sc->redo), "%s",
             gtk_editable_get_text(GTK_EDITABLE(dw->sc_redo)));
    snprintf(sc->fit, sizeof(sc->fit), "%s",
             gtk_editable_get_text(GTK_EDITABLE(dw->sc_fit)));
    snprintf(sc->zoom_in, sizeof(sc->zoom_in), "%s",
             gtk_editable_get_text(GTK_EDITABLE(dw->sc_zoom_in)));
    snprintf(sc->zoom_out, sizeof(sc->zoom_out), "%s",
             gtk_editable_get_text(GTK_EDITABLE(dw->sc_zoom_out)));
    snprintf(sc->region_confirm, sizeof(sc->region_confirm), "%s",
             gtk_editable_get_text(GTK_EDITABLE(dw->sc_region_ok)));
    snprintf(sc->region_cancel, sizeof(sc->region_cancel), "%s",
             gtk_editable_get_text(GTK_EDITABLE(dw->sc_region_cancel)));
    snprintf(sc->upload, sizeof(sc->upload), "%s",
             gtk_editable_get_text(GTK_EDITABLE(dw->sc_upload)));
    snprintf(sc->ocr, sizeof(sc->ocr), "%s",
             gtk_editable_get_text(GTK_EDITABLE(dw->sc_ocr)));
    snprintf(sc->pin, sizeof(sc->pin), "%s",
             gtk_editable_get_text(GTK_EDITABLE(dw->sc_pin)));
    snprintf(sc->crop, sizeof(sc->crop), "%s",
             gtk_editable_get_text(GTK_EDITABLE(dw->sc_crop)));
}

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
    c->upload_service = (SnapxUploadService)
        gtk_drop_down_get_selected(GTK_DROP_DOWN(dw->upload_svc_dd));
    snprintf(c->upload_imgur_client_id, sizeof(c->upload_imgur_client_id), "%s",
             gtk_editable_get_text(GTK_EDITABLE(dw->imgur_entry)));
    snprintf(c->upload_custom_url, sizeof(c->upload_custom_url), "%s",
             gtk_editable_get_text(GTK_EDITABLE(dw->custom_url_entry)));
    c->upload_copy_url = gtk_switch_get_active(GTK_SWITCH(dw->copy_url_sw));
    c->upload_auto     = gtk_switch_get_active(GTK_SWITCH(dw->auto_upload_sw));
    c->default_tool   = (SnapxAnnotationTool)
        gtk_drop_down_get_selected(GTK_DROP_DOWN(dw->tool_dd));

    c->beautify.enabled       = gtk_switch_get_active(GTK_SWITCH(dw->bf_enable_sw));
    c->beautify.padding       = (int)gtk_spin_button_get_value(
        GTK_SPIN_BUTTON(dw->bf_padding_spin));
    c->beautify.bg_type       = (SnapxBeautifyBg)
        gtk_drop_down_get_selected(GTK_DROP_DOWN(dw->bf_bg_dd));
    read_rgb_button(dw->bf_color1, &c->beautify.bg_r,
                    &c->beautify.bg_g, &c->beautify.bg_b);
    read_rgb_button(dw->bf_color2, &c->beautify.bg_r2,
                    &c->beautify.bg_g2, &c->beautify.bg_b2);
    c->beautify.corner_radius = (int)gtk_spin_button_get_value(
        GTK_SPIN_BUTTON(dw->bf_corner_spin));
    c->beautify.shadow        = gtk_switch_get_active(GTK_SWITCH(dw->bf_shadow_sw));
    c->beautify.shadow_size   = (int)gtk_spin_button_get_value(
        GTK_SPIN_BUTTON(dw->bf_shadow_spin));

    c->magnifier_enabled   = gtk_switch_get_active(GTK_SWITCH(dw->magnifier_sw));
    c->magnifier_zoom      = (int)gtk_spin_button_get_value(
        GTK_SPIN_BUTTON(dw->magnifier_zoom_spin));
    c->window_snap_enabled = gtk_switch_get_active(GTK_SWITCH(dw->window_snap_sw));
    c->close_to_tray       = gtk_switch_get_active(GTK_SWITCH(dw->close_to_tray_sw));
    c->start_in_tray       = gtk_switch_get_active(GTK_SWITCH(dw->start_in_tray_sw));
    snprintf(c->ocr_lang, sizeof(c->ocr_lang), "%s",
             gtk_editable_get_text(GTK_EDITABLE(dw->ocr_lang_entry)));

    GdkRGBA rgba = {
        c->default_color_r, c->default_color_g,
        c->default_color_b, c->default_color_a
    };
#if GTK_CHECK_VERSION(4, 10, 0)
    const GdkRGBA *picked = gtk_color_dialog_button_get_rgba(
        GTK_COLOR_DIALOG_BUTTON(dw->color_btn));
    if (picked)
        rgba = *picked;
#else
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(dw->color_btn), &rgba);
#endif
    c->default_color_r = rgba.red;
    c->default_color_g = rgba.green;
    c->default_color_b = rgba.blue;
    c->default_color_a = rgba.alpha;

    read_shortcuts_from_ui(dw, &c->shortcuts);
    char err[256];
    if (!snapx_shortcuts_validate(&c->shortcuts, err, sizeof(err))) {
        gtk_label_set_text(GTK_LABEL(dw->err_label), err);
        return;
    }
    snprintf(c->hotkey, sizeof(c->hotkey), "%s", c->shortcuts.global_capture);

    snapx_config_save(c);
    gtk_window_close(GTK_WINDOW(dw->win));
}

static gboolean on_close_request(GtkWindow *win, gpointer user_data)
{
    (void)win;
    GMainLoop *loop = (GMainLoop *)user_data;
    g_recording_entry = NULL;
    g_main_loop_quit(loop);
    return FALSE;
}

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
    snapx_config_build_path(ctx->config, ctx->config->default_format, tmp, sizeof(tmp));
    char markup[640];
    snprintf(markup, sizeof(markup), "<small><i>%s</i></small>", tmp);
    gtk_label_set_markup(GTK_LABEL(ctx->preview_label), markup);
}

static void on_pattern_changed(GtkEditable *e, gpointer data)
{
    (void)e;
    update_preview((PatternPreviewCtx *)data);
}

static GtkWidget *new_shortcut_entry(const char *value)
{
    GtkWidget *e = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(e), value);
    gtk_editable_set_editable(GTK_EDITABLE(e), FALSE);
    gtk_widget_add_css_class(e, "snapx-shortcut-entry");
    return e;
}

static GtkWidget *new_color_button(SnapxConfig *config)
{
    GdkRGBA def_color = {
        config->default_color_r, config->default_color_g,
        config->default_color_b, config->default_color_a
    };
#if GTK_CHECK_VERSION(4, 10, 0)
    GtkColorDialog *cdlg = gtk_color_dialog_new();
    gtk_color_dialog_set_title(cdlg, "Default annotation color");
    gtk_color_dialog_set_with_alpha(cdlg, TRUE);
    GtkWidget *color_btn = gtk_color_dialog_button_new(cdlg);
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(color_btn), &def_color);
    return color_btn;
#else
    GtkWidget *color_btn = gtk_color_button_new_with_rgba(&def_color);
    gtk_color_button_set_title(GTK_COLOR_BUTTON(color_btn), "Default annotation color");
    return color_btn;
#endif
}

static GtkWidget *new_rgb_button(double r, double g, double b, const char *title)
{
    GdkRGBA c = { r, g, b, 1.0 };
#if GTK_CHECK_VERSION(4, 10, 0)
    GtkColorDialog *cd = gtk_color_dialog_new();
    gtk_color_dialog_set_title(cd, title);
    gtk_color_dialog_set_with_alpha(cd, FALSE);
    GtkWidget *btn = gtk_color_dialog_button_new(cd);
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(btn), &c);
    return btn;
#else
    GtkWidget *btn = gtk_color_button_new_with_rgba(&c);
    gtk_color_button_set_title(GTK_COLOR_BUTTON(btn), title);
    return btn;
#endif
}

static void read_rgb_button(GtkWidget *btn, double *r, double *g, double *b)
{
    GdkRGBA c = { 0, 0, 0, 1 };
#if GTK_CHECK_VERSION(4, 10, 0)
    const GdkRGBA *cc = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(btn));
    if (cc) c = *cc;
#else
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(btn), &c);
#endif
    *r = c.red; *g = c.green; *b = c.blue;
}

static GtkWidget *new_help_label(const char *text)
{
    GtkWidget *lbl = gtk_label_new(text);
    gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(lbl), PANGO_WRAP_WORD);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_add_css_class(lbl, "dim-label");
    return lbl;
}

void snapx_settings_dialog_show(GtkWindow *parent, SnapxConfig *config)
{
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_recording_entry = NULL;

    GtkWidget *win = gtk_window_new();
    gtk_widget_add_css_class(win, "snapx-settings");
    gtk_window_set_title(GTK_WINDOW(win), "Preferences");
    gtk_window_set_default_size(GTK_WINDOW(win), 640, 560);
    gtk_window_set_resizable(GTK_WINDOW(win), TRUE);
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    if (parent)
        gtk_window_set_transient_for(GTK_WINDOW(win), parent);

    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(key_ctrl),
                                              GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_settings_key), NULL);
    gtk_widget_add_controller(win, key_ctrl);

    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header),
                                    gtk_label_new("Preferences"));
    gtk_window_set_titlebar(GTK_WINDOW(win), header);

    GtkWidget *btn_cancel = gtk_button_new_with_label("Cancel");
    GtkWidget *btn_ok     = gtk_button_new_with_label("Save");
    gtk_widget_add_css_class(btn_ok, "suggested-action");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), btn_cancel);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), btn_ok);
    g_signal_connect_swapped(btn_cancel, "clicked",
                              G_CALLBACK(gtk_window_close), win);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(win), vbox);

    GtkWidget *notebook = gtk_notebook_new();
    gtk_widget_set_vexpand(notebook, TRUE);
    gtk_box_append(GTK_BOX(vbox), notebook);

    GtkWidget *err_label = gtk_label_new("");
    gtk_widget_set_halign(err_label, GTK_ALIGN_START);
    gtk_widget_add_css_class(err_label, "error");
    gtk_widget_set_margin_start(err_label, 20);
    gtk_widget_set_margin_end(err_label, 20);
    gtk_widget_set_margin_bottom(err_label, 8);

    /* ── Tab: Save ─────────────────────────────────────────────────────── */
    GtkWidget *save_page = new_tab_page();
    GtkWidget *save_rows = new_pref_group("Save location");
    gtk_box_append(GTK_BOX(save_page), gtk_widget_get_parent(save_rows));

    GtkWidget *dir_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(dir_entry), config->save_dir);
    pref_row_add(save_rows, "Directory:", dir_entry);

    GtkWidget *pattern_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(pattern_entry), config->filename_pattern);
    pref_row_add(save_rows, "Filename pattern:", pattern_entry);

    gtk_box_append(GTK_BOX(save_page), new_help_label(
        "Tokens: %Y %m %d %H %M %S (date/time), %n (0001…), "
        "%d / %i / %u (1, 2, 3…), %03d / %04d (zero-padded counter), "
        "%% (literal %)"));

    GtkWidget *preview_lbl = gtk_label_new("");
    gtk_widget_set_halign(preview_lbl, GTK_ALIGN_START);
    pref_row_add(save_rows, "Next save as:", preview_lbl);

    PatternPreviewCtx *pctx = g_new(PatternPreviewCtx, 1);
    pctx->pattern_entry = pattern_entry;
    pctx->preview_label = preview_lbl;
    pctx->config        = config;
    update_preview(pctx);
    g_signal_connect(pattern_entry, "changed", G_CALLBACK(on_pattern_changed), pctx);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), wrap_tab_scroll(save_page),
                              gtk_label_new("Save"));

    /* ── Tab: Capture ──────────────────────────────────────────────────── */
    GtkWidget *cap_page = new_tab_page();
    GtkWidget *cap_rows = new_pref_group("Capture defaults");
    gtk_box_append(GTK_BOX(cap_page), gtk_widget_get_parent(cap_rows));

    const char *modes[] = { "Full Screen", "Single Monitor", "Region",
                             "Window", "Active Window", NULL };
    GtkWidget *mode_dd = gtk_drop_down_new_from_strings(modes);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(mode_dd), (guint)config->default_mode);
    pref_row_add_compact(cap_rows, "Default mode:", mode_dd);

    GtkWidget *delay_spin = gtk_spin_button_new_with_range(0, 60, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(delay_spin),
                               (double)config->default_delay);
    pref_row_add_compact(cap_rows, "Delay (seconds):", delay_spin);

    GtkWidget *cap_opt_rows = new_pref_group("Options");
    gtk_box_append(GTK_BOX(cap_page), gtk_widget_get_parent(cap_opt_rows));

    GtkWidget *cursor_sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(cursor_sw), config->show_cursor);
    pref_row_add_switch(cap_opt_rows, "Include cursor", cursor_sw);

    GtkWidget *ann_rows = new_pref_group("Annotation defaults");
    gtk_box_append(GTK_BOX(cap_page), gtk_widget_get_parent(ann_rows));

    /* Order MUST match the SnapxAnnotationTool enum values (index == enum). */
    const char *tools[] = { "Rectangle", "Arrow", "Pen", "Text",
                             "Blur", "Highlight", "Callout", "Redact",
                             "Line", "Ellipse", NULL };
    GtkWidget *tool_dd = gtk_drop_down_new_from_strings(tools);
    if ((unsigned)config->default_tool < 10u)
        gtk_drop_down_set_selected(GTK_DROP_DOWN(tool_dd),
                                   (guint)config->default_tool);
    pref_row_add_compact(ann_rows, "Default tool:", tool_dd);

    GtkWidget *color_btn = new_color_button(config);
    pref_row_add_compact(ann_rows, "Default color:", color_btn);

    GtkWidget *overlay_rows = new_pref_group("Region overlay");
    gtk_box_append(GTK_BOX(cap_page), gtk_widget_get_parent(overlay_rows));

    GtkWidget *magnifier_sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(magnifier_sw), config->magnifier_enabled);
    pref_row_add_switch(overlay_rows, "Magnifier (hold Space)", magnifier_sw);

    GtkWidget *magnifier_zoom_spin = gtk_spin_button_new_with_range(2, 16, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(magnifier_zoom_spin),
                               (double)config->magnifier_zoom);
    pref_row_add_compact(overlay_rows, "Magnifier zoom:", magnifier_zoom_spin);

    GtkWidget *window_snap_sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(window_snap_sw), config->window_snap_enabled);
    pref_row_add_switch(overlay_rows, "Snap to window", window_snap_sw);

    GtkWidget *tray_rows = new_pref_group("Background / tray");
    gtk_box_append(GTK_BOX(cap_page), gtk_widget_get_parent(tray_rows));

    GtkWidget *close_to_tray_sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(close_to_tray_sw), config->close_to_tray);
    pref_row_add_switch(tray_rows, "Close to tray (hide window)", close_to_tray_sw);

    GtkWidget *start_in_tray_sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(start_in_tray_sw), config->start_in_tray);
    pref_row_add_switch(tray_rows, "Start hidden in background", start_in_tray_sw);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), wrap_tab_scroll(cap_page),
                              gtk_label_new("Capture"));

    /* ── Tab: Output ───────────────────────────────────────────────────── */
    GtkWidget *out_page = new_tab_page();
    GtkWidget *out_rows = new_pref_group("After capture");
    gtk_box_append(GTK_BOX(out_page), gtk_widget_get_parent(out_rows));

    const char *fmts[] = { "PNG", "JPEG", "WebP", NULL };
    GtkWidget *fmt_dd = gtk_drop_down_new_from_strings(fmts);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(fmt_dd),
                                (guint)config->default_format);
    pref_row_add_compact(out_rows, "Default format:", fmt_dd);

    GtkWidget *quality_spin = gtk_spin_button_new_with_range(1, 100, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(quality_spin),
                               (double)config->jpeg_quality);
    pref_row_add_compact(out_rows, "JPEG quality:", quality_spin);

    GtkWidget *out_opt_rows = new_pref_group("Options");
    gtk_box_append(GTK_BOX(out_page), gtk_widget_get_parent(out_opt_rows));

    GtkWidget *clipboard_sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(clipboard_sw), config->auto_clipboard);
    pref_row_add_switch(out_opt_rows, "Auto copy to clipboard", clipboard_sw);

    GtkWidget *sound_sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sound_sw), config->play_sound);
    pref_row_add_switch(out_opt_rows, "Play sound on capture", sound_sw);

    /* ── Beautiful export ──────────────────────────────────────────────── */
    GtkWidget *bf_rows = new_pref_group("Beautiful export");
    gtk_box_append(GTK_BOX(out_page), gtk_widget_get_parent(bf_rows));

    GtkWidget *bf_enable_sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(bf_enable_sw), config->beautify.enabled);
    pref_row_add_switch(bf_rows, "Apply on Save / Copy / Upload", bf_enable_sw);

    GtkWidget *bf_padding_spin = gtk_spin_button_new_with_range(0, 240, 4);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(bf_padding_spin),
                              (double)config->beautify.padding);
    pref_row_add_compact(bf_rows, "Padding (px):", bf_padding_spin);

    const char *bf_bgs[] = { "Solid", "Gradient", "Transparent", NULL };
    GtkWidget *bf_bg_dd = gtk_drop_down_new_from_strings(bf_bgs);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(bf_bg_dd),
                               (guint)config->beautify.bg_type);
    pref_row_add_compact(bf_rows, "Background:", bf_bg_dd);

    GtkWidget *bf_color1 = new_rgb_button(config->beautify.bg_r,
                                          config->beautify.bg_g,
                                          config->beautify.bg_b,
                                          "Background colour");
    pref_row_add_compact(bf_rows, "Colour:", bf_color1);

    GtkWidget *bf_color2 = new_rgb_button(config->beautify.bg_r2,
                                          config->beautify.bg_g2,
                                          config->beautify.bg_b2,
                                          "Gradient end colour");
    pref_row_add_compact(bf_rows, "Gradient end:", bf_color2);

    GtkWidget *bf_corner_spin = gtk_spin_button_new_with_range(0, 64, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(bf_corner_spin),
                              (double)config->beautify.corner_radius);
    pref_row_add_compact(bf_rows, "Corner radius (px):", bf_corner_spin);

    GtkWidget *bf_shadow_sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(bf_shadow_sw), config->beautify.shadow);
    pref_row_add_switch(bf_rows, "Drop shadow", bf_shadow_sw);

    GtkWidget *bf_shadow_spin = gtk_spin_button_new_with_range(2, 80, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(bf_shadow_spin),
                              (double)config->beautify.shadow_size);
    pref_row_add_compact(bf_rows, "Shadow size (px):", bf_shadow_spin);

    gtk_box_append(GTK_BOX(out_page), new_help_label(
        "Wrap captures in a background, padding, rounded corners and a shadow. "
        "Use the Beautify… button in the main window for a live preview."));

    GtkWidget *ocr_rows = new_pref_group("OCR");
    gtk_box_append(GTK_BOX(out_page), gtk_widget_get_parent(ocr_rows));

    {
        char ocr_st[128];
        snapx_ocr_status_message(config, ocr_st, sizeof(ocr_st));
        GtkWidget *ocr_status = gtk_label_new(ocr_st);
        gtk_widget_add_css_class(ocr_status, "snapx-workflow-status");
        if (snapx_ocr_get_status(config) == SNAPX_OCR_STATUS_AVAILABLE)
            gtk_widget_add_css_class(ocr_status, "ok");
        else
            gtk_widget_add_css_class(ocr_status, "warn");
        gtk_box_append(GTK_BOX(ocr_rows), ocr_status);
    }

    GtkWidget *ocr_lang_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(ocr_lang_entry), config->ocr_lang);
    pref_row_add_compact(ocr_rows, "Tesseract language:", ocr_lang_entry);
    gtk_box_append(GTK_BOX(out_page), new_help_label(
        "OCR requires Tesseract at build time (e.g. eng, deu). "
        "Fedora: dnf install tesseract-devel, then rebuild snapx."));

    GtkWidget *upload_rows = new_pref_group("Upload");
    gtk_box_append(GTK_BOX(out_page), gtk_widget_get_parent(upload_rows));

    {
        char upload_st[128];
        snapx_upload_status_message(config, upload_st, sizeof(upload_st));
        GtkWidget *upload_status = gtk_label_new(upload_st);
        gtk_widget_add_css_class(upload_status, "snapx-workflow-status");
        if (snapx_upload_get_status(config) == SNAPX_UPLOAD_STATUS_AVAILABLE)
            gtk_widget_add_css_class(upload_status, "ok");
        else
            gtk_widget_add_css_class(upload_status, "warn");
        gtk_box_append(GTK_BOX(upload_rows), upload_status);
    }

    const char *upload_svcs[] = { "None", "Imgur", "Custom URL", NULL };
    GtkWidget *upload_svc_dd = gtk_drop_down_new_from_strings(upload_svcs);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(upload_svc_dd),
                                (guint)config->upload_service);
    pref_row_add_compact(upload_rows, "Service:", upload_svc_dd);

    GtkWidget *imgur_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(imgur_entry), config->upload_imgur_client_id);
    pref_row_add_compact(upload_rows, "Imgur client ID:", imgur_entry);

    GtkWidget *custom_url_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(custom_url_entry), config->upload_custom_url);
    pref_row_add_compact(upload_rows, "Custom URL:", custom_url_entry);

    GtkWidget *copy_url_sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(copy_url_sw), config->upload_copy_url);
    pref_row_add_switch(upload_rows, "Copy URL after upload", copy_url_sw);

    GtkWidget *auto_upload_sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(auto_upload_sw), config->upload_auto);
    pref_row_add_switch(upload_rows, "Auto upload after capture", auto_upload_sw);

    gtk_box_append(GTK_BOX(out_page), new_help_label(
        "Upload requires libcurl at build time. "
        "Fedora: dnf install libcurl-devel, then rebuild snapx. "
        "Imgur needs a client ID from api.imgur.com."));

    gtk_box_append(GTK_BOX(out_page), new_help_label(
        "Sound uses the system beep after a successful capture."));

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), wrap_tab_scroll(out_page),
                              gtk_label_new("Output"));

    /* ── Tab: Shortcuts ────────────────────────────────────────────────── */
    GtkWidget *sc_page = new_tab_page();

    gtk_box_append(GTK_BOX(sc_page), new_help_label(
        "Click Record, then press a key combination. Clear removes a binding. "
        "Global shortcuts work system-wide on X11/Windows and on Wayland via "
        "the desktop portal (approve on first launch). In-app shortcuts work "
        "while snapx is focused."));

    GtkWidget *reset_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(reset_row, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(reset_row, 4);
    GtkWidget *btn_reset = gtk_button_new_with_label("Reset to defaults");
    gtk_box_append(GTK_BOX(reset_row), btn_reset);
    gtk_box_append(GTK_BOX(sc_page), reset_row);

    const SnapxShortcuts *sc = &config->shortcuts;

    GtkWidget *grid_cap = new_shortcuts_grid();
    shortcuts_grid_add_header(grid_cap, 0);
    GtkWidget *sc_global = shortcuts_grid_add(
        grid_cap, 1, "Global (default mode)",
        new_shortcut_entry(sc->global_capture));
    GtkWidget *sc_capture_full = shortcuts_grid_add(
        grid_cap, 2, "Full screen",
        new_shortcut_entry(sc->capture_fullscreen));
    GtkWidget *sc_capture_monitor = shortcuts_grid_add(
        grid_cap, 3, "Monitor",
        new_shortcut_entry(sc->capture_monitor));
    GtkWidget *sc_capture_region = shortcuts_grid_add(
        grid_cap, 4, "Region",
        new_shortcut_entry(sc->capture_region));
    GtkWidget *sc_capture_window = shortcuts_grid_add(
        grid_cap, 5, "Window",
        new_shortcut_entry(sc->capture_window));
    shortcuts_section_add(sc_page, "Global & capture", grid_cap, TRUE);

    GtkWidget *grid_ed = new_shortcuts_grid();
    shortcuts_grid_add_header(grid_ed, 0);
    GtkWidget *sc_save = shortcuts_grid_add(
        grid_ed, 1, "Save", new_shortcut_entry(sc->save));
    GtkWidget *sc_copy = shortcuts_grid_add(
        grid_ed, 2, "Copy", new_shortcut_entry(sc->copy));
    GtkWidget *sc_undo = shortcuts_grid_add(
        grid_ed, 3, "Undo", new_shortcut_entry(sc->undo));
    GtkWidget *sc_redo = shortcuts_grid_add(
        grid_ed, 4, "Redo", new_shortcut_entry(sc->redo));
    GtkWidget *sc_fit = shortcuts_grid_add(
        grid_ed, 5, "Fit view", new_shortcut_entry(sc->fit));
    GtkWidget *sc_zoom_in = shortcuts_grid_add(
        grid_ed, 6, "Zoom in", new_shortcut_entry(sc->zoom_in));
    GtkWidget *sc_zoom_out = shortcuts_grid_add(
        grid_ed, 7, "Zoom out", new_shortcut_entry(sc->zoom_out));
    GtkWidget *sc_upload = shortcuts_grid_add(
        grid_ed, 8, "Upload", new_shortcut_entry(sc->upload));
    GtkWidget *sc_ocr = shortcuts_grid_add(
        grid_ed, 9, "Copy text (OCR)", new_shortcut_entry(sc->ocr));
    GtkWidget *sc_pin = shortcuts_grid_add(
        grid_ed, 10, "Pin to screen", new_shortcut_entry(sc->pin));
    GtkWidget *sc_crop = shortcuts_grid_add(
        grid_ed, 11, "Crop", new_shortcut_entry(sc->crop));
    shortcuts_section_add(sc_page, "Editor", grid_ed, FALSE);

    GtkWidget *grid_ov = new_shortcuts_grid();
    shortcuts_grid_add_header(grid_ov, 0);
    GtkWidget *sc_region_ok = shortcuts_grid_add(
        grid_ov, 1, "Confirm",
        new_shortcut_entry(sc->region_confirm));
    GtkWidget *sc_region_cancel = shortcuts_grid_add(
        grid_ov, 2, "Cancel",
        new_shortcut_entry(sc->region_cancel));
    shortcuts_section_add(sc_page, "Region overlay", grid_ov, FALSE);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), wrap_tab_scroll(sc_page),
                              gtk_label_new("Shortcuts"));

    DlgWidgets *dw = g_new(DlgWidgets, 1);
    dw->config           = config;
    dw->win              = win;
    dw->dir_entry        = dir_entry;
    dw->pattern_entry    = pattern_entry;
    dw->mode_dd          = mode_dd;
    dw->delay_spin       = delay_spin;
    dw->cursor_sw        = cursor_sw;
    dw->fmt_dd           = fmt_dd;
    dw->quality_spin     = quality_spin;
    dw->clipboard_sw     = clipboard_sw;
    dw->sound_sw         = sound_sw;
    dw->upload_svc_dd    = upload_svc_dd;
    dw->imgur_entry      = imgur_entry;
    dw->custom_url_entry = custom_url_entry;
    dw->copy_url_sw      = copy_url_sw;
    dw->auto_upload_sw   = auto_upload_sw;
    dw->tool_dd          = tool_dd;
    dw->color_btn        = color_btn;
    dw->bf_enable_sw     = bf_enable_sw;
    dw->bf_padding_spin  = bf_padding_spin;
    dw->bf_bg_dd         = bf_bg_dd;
    dw->bf_color1        = bf_color1;
    dw->bf_color2        = bf_color2;
    dw->bf_corner_spin   = bf_corner_spin;
    dw->bf_shadow_sw     = bf_shadow_sw;
    dw->bf_shadow_spin   = bf_shadow_spin;
    dw->sc_global        = sc_global;
    dw->sc_capture_full  = sc_capture_full;
    dw->sc_capture_monitor = sc_capture_monitor;
    dw->sc_capture_region  = sc_capture_region;
    dw->sc_capture_window  = sc_capture_window;
    dw->sc_save          = sc_save;
    dw->sc_copy          = sc_copy;
    dw->sc_undo          = sc_undo;
    dw->sc_redo          = sc_redo;
    dw->sc_fit           = sc_fit;
    dw->sc_zoom_in       = sc_zoom_in;
    dw->sc_zoom_out      = sc_zoom_out;
    dw->sc_region_ok     = sc_region_ok;
    dw->sc_region_cancel = sc_region_cancel;
    dw->sc_upload        = sc_upload;
    dw->sc_ocr           = sc_ocr;
    dw->sc_pin           = sc_pin;
    dw->sc_crop          = sc_crop;
    dw->magnifier_sw     = magnifier_sw;
    dw->magnifier_zoom_spin = magnifier_zoom_spin;
    dw->window_snap_sw   = window_snap_sw;
    dw->close_to_tray_sw = close_to_tray_sw;
    dw->start_in_tray_sw = start_in_tray_sw;
    dw->ocr_lang_entry   = ocr_lang_entry;
    dw->err_label        = err_label;
    dw->loop             = loop;

    g_signal_connect(btn_reset, "clicked", G_CALLBACK(on_reset_shortcuts), dw);

    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(footer, "snapx-settings-footer");
    gtk_box_append(GTK_BOX(footer), err_label);
    gtk_box_append(GTK_BOX(vbox), footer);

    g_signal_connect(btn_ok, "clicked", G_CALLBACK(on_ok_clicked), dw);
    g_signal_connect(win, "close-request", G_CALLBACK(on_close_request), loop);

    gtk_widget_set_visible(win, TRUE);
    g_main_loop_run(loop);

    g_free(pctx);
    g_free(dw);
    g_main_loop_unref(loop);
}
