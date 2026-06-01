/**
 * @file toolbar.c
 * @brief Annotation toolbar — GtkToggleButtons with CSS active states,
 *        colour picker, undo/redo, and annotation dirty tracking.
 *
 * Tool buttons use GtkToggleButton so the selected tool is always
 * visually highlighted.  The CSS class `snapx-tool` + `:checked`
 * gives the accent-colour highlight from snapx.css.
 *
 * Layout:
 *   [Rect] [Arrow] [Pen] [Text] [Blur] [Hi] | [Color…] ── spacer ── [Undo] [Redo]
 */

#include "toolbar.h"
#include "window_main.h"
#include "../annotation/canvas.h"
#include "../annotation/annotation.h"
#include "../utils/config.h"

#include <string.h>
#include <stdio.h>

/* ─── Module state ───────────────────────────────────────────────────────── */

typedef struct {
    SnapxAnnotationCanvas *canvas;
    GtkWidget             *drawing_area;
    SnapxAnnotationTool    active_tool;
    GdkRGBA                active_color;

    /* Stroke tracking */
    gboolean               in_stroke;
    double                 stroke_start_x, stroke_start_y;

    /* Tool button references so we can set active state */
    GtkWidget             *tool_btns[10];
    GtkWidget             *color_btn;

    /* Eyedropper: when armed, the next canvas click samples a pixel colour
     * instead of starting a stroke. */
    GtkWidget             *eyedropper_btn;
    gboolean               eyedropper_armed;

    /* Annotation dirty flag (set on every stroke commit) */
    gboolean               annot_dirty;

    gboolean               input_enabled;
    GtkWidget             *text_popover;
    GtkWidget             *text_entry;
    double                 text_ix, text_iy;
} ToolbarState;

static ToolbarState g_tb;

static void stroke_point(double wx, double wy, double *ix, double *iy);

static void commit_text_from_entry(GtkEntry *entry, gpointer data)
{
    (void)data;
    if (!g_tb.canvas) return;
    const char *txt = gtk_editable_get_text(GTK_EDITABLE(entry));
    snapx_canvas_stroke_end_text(g_tb.canvas, g_tb.text_ix, g_tb.text_iy, txt);
    g_tb.annot_dirty = TRUE;
    if (g_tb.text_popover)
        gtk_popover_popdown(GTK_POPOVER(g_tb.text_popover));
    snapx_main_schedule_redraw();
}

static void show_text_popover(GtkWidget *anchor, double wx, double wy)
{
    stroke_point(wx, wy, &g_tb.text_ix, &g_tb.text_iy);
    if (!g_tb.text_popover) {
        g_tb.text_popover = gtk_popover_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_margin_start(box, 8);
        gtk_widget_set_margin_end(box, 8);
        gtk_widget_set_margin_top(box, 8);
        gtk_widget_set_margin_bottom(box, 8);
        g_tb.text_entry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(g_tb.text_entry), "Enter text…");
        g_signal_connect(g_tb.text_entry, "activate",
                         G_CALLBACK(commit_text_from_entry), NULL);
        gtk_box_append(GTK_BOX(box), g_tb.text_entry);
        gtk_popover_set_child(GTK_POPOVER(g_tb.text_popover), box);
    }
    gtk_widget_set_parent(g_tb.text_popover, anchor);
    gtk_editable_set_text(GTK_EDITABLE(g_tb.text_entry), "");
    gtk_popover_popup(GTK_POPOVER(g_tb.text_popover));
    gtk_widget_grab_focus(g_tb.text_entry);
}

/* ─── Tool descriptor ────────────────────────────────────────────────────── */

typedef struct {
    SnapxAnnotationTool id;
    const char         *label;
    const char         *tooltip;
} ToolDef;

static const ToolDef TOOLS[] = {
    { SNAPX_TOOL_RECT,      "Rect",    "Draw rectangle (R)"           },
    { SNAPX_TOOL_ELLIPSE,   "Ellipse", "Draw ellipse / circle"        },
    { SNAPX_TOOL_LINE,      "Line",    "Draw straight line"           },
    { SNAPX_TOOL_ARROW,     "Arrow",   "Draw arrow (A)"               },
    { SNAPX_TOOL_PEN,       "Pen",     "Free-hand pen (P)"            },
    { SNAPX_TOOL_TEXT,      "Text",    "Add text label (T)"           },
    { SNAPX_TOOL_BLUR,      "Blur",    "Blur / censor region (B)"     },
    { SNAPX_TOOL_HIGHLIGHT, "Hi-lite", "Highlight region (H)"         },
    { SNAPX_TOOL_CALLOUT,   "Callout", "Numbered callout (N)"         },
    { SNAPX_TOOL_REDACT,    "Redact",  "Solid black redaction"        },
};
#define NUM_TOOLS ((int)(sizeof(TOOLS)/sizeof(TOOLS[0])))

/* ─── Tool selection ─────────────────────────────────────────────────────── */

/* GtkToggleButton emits "toggled" when clicked — but that fires for both
 * the newly-active and the de-activating button.  We only care about the
 * button becoming active. */
static void on_tool_toggled(GtkToggleButton *btn, gpointer data)
{
    if (!gtk_toggle_button_get_active(btn)) return;   /* ignore de-activation */
    int idx = GPOINTER_TO_INT(data);
    g_tb.active_tool = TOOLS[idx].id;
    if (g_tb.canvas)
        snapx_canvas_set_tool(g_tb.canvas, TOOLS[idx].id);
    /* Keep others un-checked */
    for (int i = 0; i < NUM_TOOLS; i++) {
        if (i != idx && g_tb.tool_btns[i])
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_tb.tool_btns[i]), FALSE);
    }
}

/* ─── Color picker ───────────────────────────────────────────────────────── */

/** Set the active annotation colour and sync the canvas + colour button widget. */
static void apply_active_color(const GdkRGBA *c)
{
    if (!c) return;
    g_tb.active_color = *c;
    if (g_tb.canvas)
        snapx_canvas_set_color(g_tb.canvas, &g_tb.active_color);
    if (g_tb.color_btn) {
#if GTK_CHECK_VERSION(4, 10, 0)
        gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(g_tb.color_btn),
                                         &g_tb.active_color);
#else
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g_tb.color_btn),
                                   &g_tb.active_color);
#endif
    }
}

static void on_color_notify(GObject *obj, GParamSpec *pspec, gpointer data)
{
    (void)pspec; (void)data;
#if GTK_CHECK_VERSION(4, 10, 0)
    const GdkRGBA *c = gtk_color_dialog_button_get_rgba(
                            GTK_COLOR_DIALOG_BUTTON(obj));
    if (c) g_tb.active_color = *c;
#else
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(obj), &g_tb.active_color);
#endif
    if (g_tb.canvas)
        snapx_canvas_set_color(g_tb.canvas, &g_tb.active_color);
}

/* ─── Eyedropper ─────────────────────────────────────────────────────────── */

static void set_eyedropper_armed(gboolean armed)
{
    g_tb.eyedropper_armed = armed;
    if (g_tb.eyedropper_btn)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_tb.eyedropper_btn), armed);
}

static void on_eyedropper_toggled(GtkToggleButton *btn, gpointer data)
{
    (void)data;
    g_tb.eyedropper_armed = gtk_toggle_button_get_active(btn);
}

/** Sample the image colour under widget coords (wx, wy) and adopt it.
 *  @return TRUE if a colour was sampled (eyedropper consumed the click). */
static gboolean eyedropper_try_sample(double wx, double wy)
{
    if (!g_tb.eyedropper_armed) return FALSE;
    double ix, iy;
    stroke_point(wx, wy, &ix, &iy);
    GdkRGBA c;
    if (snapx_main_sample_color(ix, iy, &c))
        apply_active_color(&c);
    set_eyedropper_armed(FALSE);
    return TRUE;
}

/* ─── Undo / Redo ────────────────────────────────────────────────────────── */

static void on_undo(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (g_tb.canvas)  snapx_canvas_undo(g_tb.canvas);
    g_tb.annot_dirty = TRUE;
    snapx_main_schedule_redraw();
}

static void on_redo(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (g_tb.canvas)  snapx_canvas_redo(g_tb.canvas);
    g_tb.annot_dirty = TRUE;
    snapx_main_schedule_redraw();
}

/* ─── Canvas gesture / event handlers ───────────────────────────────────── */

static void stroke_point(double wx, double wy, double *ix, double *iy)
{
    snapx_main_widget_to_image(wx, wy, ix, iy);
}

#ifdef SNAPX_USE_GTK4

static void on_canvas_press(GtkGestureClick *g, int n, double x, double y,
                              gpointer d)
{
    (void)g; (void)n; (void)d;
    if (eyedropper_try_sample(x, y)) return;
    if (!g_tb.canvas || !g_tb.input_enabled) return;
    double ix, iy;
    stroke_point(x, y, &ix, &iy);
    g_tb.in_stroke      = TRUE;
    g_tb.stroke_start_x = ix;
    g_tb.stroke_start_y = iy;
    snapx_canvas_stroke_begin(g_tb.canvas, ix, iy);
}

static void on_canvas_release(GtkGestureClick *g, int n, double x, double y,
                                gpointer d)
{
    (void)n; (void)d;
    if (!g_tb.canvas || !g_tb.in_stroke || !g_tb.input_enabled) return;
    if (g_tb.active_tool == SNAPX_TOOL_TEXT) {
        g_tb.in_stroke = FALSE;
        GtkWidget *anchor = gtk_event_controller_get_widget(
            GTK_EVENT_CONTROLLER(g));
        show_text_popover(anchor, x, y);
        return;
    }
    double ix, iy;
    stroke_point(x, y, &ix, &iy);
    SnapxAnnotationTool tool = g_tb.active_tool;
    g_tb.in_stroke = FALSE;
    if (tool == SNAPX_TOOL_BLUR) {
        snapx_main_apply_blur_region(g_tb.stroke_start_x, g_tb.stroke_start_y,
                                     ix, iy);
        snapx_canvas_stroke_cancel(g_tb.canvas);
        snapx_main_schedule_redraw();
        return;
    }
    g_tb.annot_dirty = TRUE;
    snapx_canvas_stroke_end(g_tb.canvas, ix, iy);
    snapx_main_schedule_redraw();
}

static void on_canvas_motion(GtkEventControllerMotion *ctrl, double x, double y,
                               gpointer d)
{
    (void)ctrl; (void)d;
    if (!g_tb.canvas || !g_tb.in_stroke) return;
    double ix, iy;
    stroke_point(x, y, &ix, &iy);
    snapx_canvas_stroke_update(g_tb.canvas, ix, iy);
    snapx_main_schedule_redraw();
}

#else /* GTK3 */

static gboolean on_canvas_press_gtk3(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
    (void)d;
    if (ev->button == 1 && eyedropper_try_sample(ev->x, ev->y)) return FALSE;
    if (!g_tb.canvas || !g_tb.input_enabled || ev->button != 1) return FALSE;
    double ix, iy;
    stroke_point(ev->x, ev->y, &ix, &iy);
    g_tb.in_stroke = TRUE;
    g_tb.stroke_start_x = ix;
    g_tb.stroke_start_y = iy;
    snapx_canvas_stroke_begin(g_tb.canvas, ix, iy);
    return FALSE;
}

static gboolean on_canvas_release_gtk3(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
    (void)d;
    if (!g_tb.canvas || !g_tb.in_stroke || !g_tb.input_enabled || ev->button != 1)
        return FALSE;
    if (g_tb.active_tool == SNAPX_TOOL_TEXT) {
        g_tb.in_stroke = FALSE;
        show_text_popover(w, ev->x, ev->y);
        return FALSE;
    }
    double ix, iy;
    stroke_point(ev->x, ev->y, &ix, &iy);
    SnapxAnnotationTool tool = g_tb.active_tool;
    g_tb.in_stroke = FALSE;
    if (tool == SNAPX_TOOL_BLUR) {
        snapx_main_apply_blur_region(g_tb.stroke_start_x, g_tb.stroke_start_y,
                                     ix, iy);
        snapx_canvas_stroke_cancel(g_tb.canvas);
        snapx_main_schedule_redraw();
        return FALSE;
    }
    g_tb.annot_dirty = TRUE;
    snapx_canvas_stroke_end(g_tb.canvas, ix, iy);
    snapx_main_schedule_redraw();
    return FALSE;
}

static gboolean on_canvas_motion_gtk3(GtkWidget *w, GdkEventMotion *ev, gpointer d)
{
    (void)w; (void)d;
    if (!g_tb.canvas || !g_tb.in_stroke) return FALSE;
    double ix, iy;
    stroke_point(ev->x, ev->y, &ix, &iy);
    snapx_canvas_stroke_update(g_tb.canvas, ix, iy);
    snapx_main_schedule_redraw();
    return FALSE;
}

#endif /* GTK3 */

/* ─── Public API ─────────────────────────────────────────────────────────── */

void snapx_toolbar_connect_canvas_events(GtkWidget *drawing_area,
                                          SnapxAnnotationCanvas *canvas)
{
    g_tb.canvas       = canvas;
    g_tb.drawing_area = drawing_area;

#ifdef SNAPX_USE_GTK4
    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed",  G_CALLBACK(on_canvas_press),   NULL);
    g_signal_connect(click, "released", G_CALLBACK(on_canvas_release), NULL);
    gtk_widget_add_controller(drawing_area, GTK_EVENT_CONTROLLER(click));

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_canvas_motion), NULL);
    gtk_widget_add_controller(drawing_area, motion);
#else
    gtk_widget_add_events(drawing_area,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(drawing_area, "button-press-event",
                     G_CALLBACK(on_canvas_press_gtk3),   NULL);
    g_signal_connect(drawing_area, "button-release-event",
                     G_CALLBACK(on_canvas_release_gtk3), NULL);
    g_signal_connect(drawing_area, "motion-notify-event",
                     G_CALLBACK(on_canvas_motion_gtk3),  NULL);
#endif
}

void snapx_toolbar_set_canvas(SnapxAnnotationCanvas *canvas,
                               GtkWidget             *drawing_area)
{
    g_tb.canvas       = canvas;
    g_tb.drawing_area = drawing_area;
    g_tb.annot_dirty  = FALSE;
    /* Re-apply the active tool to the new canvas */
    if (canvas) {
        snapx_canvas_set_tool(canvas, g_tb.active_tool);
        snapx_canvas_set_color(canvas, &g_tb.active_color);
    }
}

gboolean snapx_toolbar_annot_dirty(void)
{
    return g_tb.annot_dirty;
}

void snapx_toolbar_annot_clear_dirty(void)
{
    g_tb.annot_dirty = FALSE;
}

gboolean snapx_toolbar_in_stroke(void)
{
    return g_tb.in_stroke;
}

void snapx_toolbar_set_input_enabled(gboolean enabled)
{
    g_tb.input_enabled = enabled;
}

/* ─── Widget construction ────────────────────────────────────────────────── */

static void apply_tool_selection(SnapxAnnotationTool tool)
{
    g_tb.active_tool = tool;
    for (int i = 0; i < NUM_TOOLS; i++) {
        if (!g_tb.tool_btns[i]) continue;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_tb.tool_btns[i]),
                                     TOOLS[i].id == tool);
    }
    if (g_tb.canvas)
        snapx_canvas_set_tool(g_tb.canvas, tool);
}

void snapx_toolbar_apply_config(const SnapxConfig *config)
{
    if (!config) return;

    g_tb.active_color = (GdkRGBA){
        config->default_color_r, config->default_color_g,
        config->default_color_b, config->default_color_a
    };
    if (g_tb.color_btn) {
#if GTK_CHECK_VERSION(4, 10, 0)
        gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(g_tb.color_btn),
                                        &g_tb.active_color);
#else
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g_tb.color_btn),
                                    &g_tb.active_color);
#endif
    }
    if (g_tb.canvas)
        snapx_canvas_set_color(g_tb.canvas, &g_tb.active_color);

    SnapxAnnotationTool tool = config->default_tool;
    if ((unsigned)tool >= (unsigned)NUM_TOOLS)
        tool = SNAPX_TOOL_RECT;
    apply_tool_selection(tool);
}

GtkWidget *snapx_toolbar_create(SnapxAnnotationCanvas *canvas,
                                 GtkWidget *drawing_area,
                                 const SnapxConfig *config)
{
    g_tb.canvas        = canvas;
    g_tb.drawing_area  = drawing_area;
    g_tb.active_tool   = SNAPX_TOOL_RECT;
    g_tb.input_enabled = TRUE;
    g_tb.active_color  = (GdkRGBA){ 0.96, 0.26, 0.26, 1.0 };
    g_tb.annot_dirty   = FALSE;
    g_tb.color_btn     = NULL;
    g_tb.eyedropper_btn   = NULL;
    g_tb.eyedropper_armed = FALSE;
    memset(g_tb.tool_btns, 0, sizeof(g_tb.tool_btns));

    if (config) {
        g_tb.active_color = (GdkRGBA){
            config->default_color_r, config->default_color_g,
            config->default_color_b, config->default_color_a
        };
        if ((unsigned)config->default_tool < (unsigned)NUM_TOOLS)
            g_tb.active_tool = config->default_tool;
    }

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_widget_add_css_class(bar, "snapx-toolbar");
    gtk_widget_set_margin_start(bar,   6);
    gtk_widget_set_margin_end(bar,     6);
    gtk_widget_set_margin_top(bar,     3);
    gtk_widget_set_margin_bottom(bar,  3);

    /* ── Tool toggle buttons ─────────────────────────────────────────────── */
    for (int i = 0; i < NUM_TOOLS; i++) {
        GtkWidget *btn = gtk_toggle_button_new_with_label(TOOLS[i].label);
        gtk_widget_add_css_class(btn, "snapx-tool");
        gtk_widget_set_tooltip_text(btn, TOOLS[i].tooltip);
        g_signal_connect(btn, "toggled", G_CALLBACK(on_tool_toggled),
                         GINT_TO_POINTER(i));
#ifdef SNAPX_USE_GTK4
        gtk_box_append(GTK_BOX(bar), btn);
#else
        gtk_box_pack_start(GTK_BOX(bar), btn, FALSE, FALSE, 1);
#endif
        g_tb.tool_btns[i] = btn;
    }
    {
        int active_idx = 0;
        for (int i = 0; i < NUM_TOOLS; i++) {
            if (TOOLS[i].id == g_tb.active_tool) {
                active_idx = i;
                break;
            }
        }
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_tb.tool_btns[active_idx]), TRUE);
    }

    /* ── Separator ───────────────────────────────────────────────────────── */
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_margin_start(sep, 4);
    gtk_widget_set_margin_end(sep, 4);
#ifdef SNAPX_USE_GTK4
    gtk_box_append(GTK_BOX(bar), sep);
#else
    gtk_box_pack_start(GTK_BOX(bar), sep, FALSE, FALSE, 4);
#endif

    /* ── Color button ────────────────────────────────────────────────────── */
    GtkWidget *color_btn;
#if GTK_CHECK_VERSION(4, 10, 0)
    GtkColorDialog *cdlg = gtk_color_dialog_new();
    gtk_color_dialog_set_title(cdlg, "Annotation color");
    gtk_color_dialog_set_with_alpha(cdlg, FALSE);
    color_btn = gtk_color_dialog_button_new(cdlg);
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(color_btn),
                                      &g_tb.active_color);
    g_signal_connect(color_btn, "notify::rgba", G_CALLBACK(on_color_notify), NULL);
#else
    color_btn = gtk_color_button_new_with_rgba(&g_tb.active_color);
    gtk_color_button_set_title(GTK_COLOR_BUTTON(color_btn), "Annotation color");
    g_signal_connect(color_btn, "color-set", G_CALLBACK(on_color_notify), NULL);
#endif
    gtk_widget_set_tooltip_text(color_btn, "Annotation color");
    g_tb.color_btn = color_btn;
#ifdef SNAPX_USE_GTK4
    gtk_box_append(GTK_BOX(bar), color_btn);
#else
    gtk_box_pack_start(GTK_BOX(bar), color_btn, FALSE, FALSE, 2);
#endif

    /* ── Eyedropper ──────────────────────────────────────────────────────── */
    GtkWidget *eyedropper = gtk_toggle_button_new_with_label("Pick");
    gtk_widget_add_css_class(eyedropper, "snapx-tool");
    gtk_widget_set_tooltip_text(eyedropper,
        "Eyedropper — click the image to sample a colour");
    g_signal_connect(eyedropper, "toggled", G_CALLBACK(on_eyedropper_toggled), NULL);
    g_tb.eyedropper_btn = eyedropper;
#ifdef SNAPX_USE_GTK4
    gtk_box_append(GTK_BOX(bar), eyedropper);
#else
    gtk_box_pack_start(GTK_BOX(bar), eyedropper, FALSE, FALSE, 2);
#endif

    /* ── Flexible spacer ─────────────────────────────────────────────────── */
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
#ifdef SNAPX_USE_GTK4
    gtk_box_append(GTK_BOX(bar), spacer);
#else
    gtk_box_pack_start(GTK_BOX(bar), spacer, TRUE, TRUE, 0);
#endif

    /* ── Undo / Redo ─────────────────────────────────────────────────────── */
    GtkWidget *btn_undo = gtk_button_new_with_label("Undo");
    GtkWidget *btn_redo = gtk_button_new_with_label("Redo");
    gtk_widget_add_css_class(btn_undo, "snapx-undoredo");
    gtk_widget_add_css_class(btn_redo, "snapx-undoredo");
    gtk_widget_set_tooltip_text(btn_undo, "Undo last annotation  (Ctrl+Z)");
    gtk_widget_set_tooltip_text(btn_redo, "Redo annotation  (Ctrl+Y)");
    g_signal_connect(btn_undo, "clicked", G_CALLBACK(on_undo), NULL);
    g_signal_connect(btn_redo, "clicked", G_CALLBACK(on_redo), NULL);
#ifdef SNAPX_USE_GTK4
    gtk_box_append(GTK_BOX(bar), btn_undo);
    gtk_box_append(GTK_BOX(bar), btn_redo);
#else
    gtk_box_pack_end(GTK_BOX(bar), btn_redo, FALSE, FALSE, 2);
    gtk_box_pack_end(GTK_BOX(bar), btn_undo, FALSE, FALSE, 2);
#endif

    return bar;
}
