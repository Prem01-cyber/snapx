/**
 * @file settings_dialog.c
 * @brief Settings dialog — preferences including filename patterns and shortcuts.
 */

#include "settings_dialog.h"
#include "../utils/config.h"
#include "../utils/shortcut.h"

#include <string.h>
#include <stdio.h>

static GtkWidget *g_recording_entry = NULL;

/* ─── Layout helpers ─────────────────────────────────────────────────────── */

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
        char markup[128];
        snprintf(markup, sizeof(markup), "<b>%s</b>", title);
        GtkWidget *lbl = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(lbl), markup);
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

static void pref_row_add(GtkWidget *rows, const char *label, GtkWidget *control)
{
    gtk_box_append(GTK_BOX(rows), make_pref_row(label, control));
}

static void pref_row_add_widget(GtkWidget *rows, GtkWidget *row)
{
    gtk_box_append(GTK_BOX(rows), row);
}

static GtkWidget *new_record_button(GtkWidget *entry);

static void on_clear_shortcut(GtkButton *btn, gpointer entry)
{
    (void)btn;
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    if (g_recording_entry == entry)
        g_recording_entry = NULL;
    gtk_widget_remove_css_class(GTK_WIDGET(entry), "snapx-recording");
}

static GtkWidget *make_shortcut_row(const char *label, GtkWidget *entry)
{
    GtkWidget *record = new_record_button(entry);
    GtkWidget *clear  = gtk_button_new_with_label("Clear");
    g_signal_connect(clear, "clicked", G_CALLBACK(on_clear_shortcut), entry);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_hexpand(entry, TRUE);
    gtk_box_append(GTK_BOX(box), entry);
    gtk_box_append(GTK_BOX(box), record);
    gtk_box_append(GTK_BOX(box), clear);
    return make_pref_row(label, box);
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
    GtkWidget   *tool_dd;
    GtkWidget   *color_btn;
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
    c->default_tool   = (SnapxAnnotationTool)
        gtk_drop_down_get_selected(GTK_DROP_DOWN(dw->tool_dd));

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

static GtkWidget *new_record_button(GtkWidget *entry)
{
    GtkWidget *b = gtk_button_new_with_label("Record");
    g_signal_connect(b, "clicked", G_CALLBACK(on_record_clicked), entry);
    return b;
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
    pref_row_add(cap_rows, "Default mode:", mode_dd);

    GtkWidget *delay_spin = gtk_spin_button_new_with_range(0, 60, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(delay_spin),
                               (double)config->default_delay);
    pref_row_add(cap_rows, "Delay (seconds):", delay_spin);

    GtkWidget *cursor_sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(cursor_sw), config->show_cursor);
    pref_row_add(cap_rows, "Include cursor:", cursor_sw);

    GtkWidget *ann_rows = new_pref_group("Annotation defaults");
    gtk_box_append(GTK_BOX(cap_page), gtk_widget_get_parent(ann_rows));

    const char *tools[] = { "Rectangle", "Arrow", "Pen", "Text",
                             "Blur", "Highlight", NULL };
    GtkWidget *tool_dd = gtk_drop_down_new_from_strings(tools);
    if ((unsigned)config->default_tool < 6u)
        gtk_drop_down_set_selected(GTK_DROP_DOWN(tool_dd),
                                   (guint)config->default_tool);
    pref_row_add(ann_rows, "Default tool:", tool_dd);

    GtkWidget *color_btn = new_color_button(config);
    pref_row_add(ann_rows, "Default color:", color_btn);

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
    pref_row_add(out_rows, "Default format:", fmt_dd);

    GtkWidget *quality_spin = gtk_spin_button_new_with_range(1, 100, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(quality_spin),
                               (double)config->jpeg_quality);
    pref_row_add(out_rows, "JPEG quality:", quality_spin);

    GtkWidget *clipboard_sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(clipboard_sw), config->auto_clipboard);
    pref_row_add(out_rows, "Auto copy to clipboard:", clipboard_sw);

    GtkWidget *sound_sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sound_sw), config->play_sound);
    pref_row_add(out_rows, "Play sound on capture:", sound_sw);

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
    GtkWidget *btn_reset = gtk_button_new_with_label("Reset to defaults");
    gtk_box_append(GTK_BOX(reset_row), btn_reset);
    gtk_box_append(GTK_BOX(sc_page), reset_row);

    const SnapxShortcuts *sc = &config->shortcuts;

    GtkWidget *sc_cap = new_pref_group("Global & capture");
    gtk_box_append(GTK_BOX(sc_page), gtk_widget_get_parent(sc_cap));
    GtkWidget *sc_global = new_shortcut_entry(sc->global_capture);
    pref_row_add_widget(sc_cap, make_shortcut_row("Global (default mode):", sc_global));
    GtkWidget *sc_capture_full = new_shortcut_entry(sc->capture_fullscreen);
    pref_row_add_widget(sc_cap, make_shortcut_row("Full screen:", sc_capture_full));
    GtkWidget *sc_capture_monitor = new_shortcut_entry(sc->capture_monitor);
    pref_row_add_widget(sc_cap, make_shortcut_row("Monitor:", sc_capture_monitor));
    GtkWidget *sc_capture_region = new_shortcut_entry(sc->capture_region);
    pref_row_add_widget(sc_cap, make_shortcut_row("Region:", sc_capture_region));
    GtkWidget *sc_capture_window = new_shortcut_entry(sc->capture_window);
    pref_row_add_widget(sc_cap, make_shortcut_row("Window:", sc_capture_window));

    GtkWidget *sc_ed = new_pref_group("Editor");
    gtk_box_append(GTK_BOX(sc_page), gtk_widget_get_parent(sc_ed));
    GtkWidget *sc_save = new_shortcut_entry(sc->save);
    pref_row_add_widget(sc_ed, make_shortcut_row("Save:", sc_save));
    GtkWidget *sc_copy = new_shortcut_entry(sc->copy);
    pref_row_add_widget(sc_ed, make_shortcut_row("Copy:", sc_copy));
    GtkWidget *sc_undo = new_shortcut_entry(sc->undo);
    pref_row_add_widget(sc_ed, make_shortcut_row("Undo:", sc_undo));
    GtkWidget *sc_redo = new_shortcut_entry(sc->redo);
    pref_row_add_widget(sc_ed, make_shortcut_row("Redo:", sc_redo));
    GtkWidget *sc_fit = new_shortcut_entry(sc->fit);
    pref_row_add_widget(sc_ed, make_shortcut_row("Fit view:", sc_fit));
    GtkWidget *sc_zoom_in = new_shortcut_entry(sc->zoom_in);
    pref_row_add_widget(sc_ed, make_shortcut_row("Zoom in:", sc_zoom_in));
    GtkWidget *sc_zoom_out = new_shortcut_entry(sc->zoom_out);
    pref_row_add_widget(sc_ed, make_shortcut_row("Zoom out:", sc_zoom_out));

    GtkWidget *sc_ov = new_pref_group("Region overlay");
    gtk_box_append(GTK_BOX(sc_page), gtk_widget_get_parent(sc_ov));
    GtkWidget *sc_region_ok = new_shortcut_entry(sc->region_confirm);
    pref_row_add_widget(sc_ov, make_shortcut_row("Confirm:", sc_region_ok));
    GtkWidget *sc_region_cancel = new_shortcut_entry(sc->region_cancel);
    pref_row_add_widget(sc_ov, make_shortcut_row("Cancel:", sc_region_cancel));

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
    dw->tool_dd          = tool_dd;
    dw->color_btn        = color_btn;
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
