/**
 * @file toolbar.c
 * @brief Annotation toolbar: tool selection, color picker, undo/redo.
 *
 * Tool buttons:
 *   [Rectangle] [Arrow] [Pen] [Text] [Blur] [Highlight] [Color…] | [Undo] [Redo]
 *
 * Selected tool and color are stored in a module-level ToolbarState shared
 * with the canvas event handlers.
 */

#include "toolbar.h"
#include "../annotation/canvas.h"
#include "../annotation/annotation.h"

#include <string.h>
#include <stdio.h>

/* ─── Toolbar state (module-level so canvas events can read it) ─────────── */

typedef struct {
    SnapxAnnotationCanvas *canvas;
    GtkWidget             *drawing_area;
    SnapxAnnotationTool    active_tool;
    GdkRGBA                active_color;

    /* Stroke tracking */
    gboolean               in_stroke;
    double                 stroke_start_x;
    double                 stroke_start_y;
} ToolbarState;

static ToolbarState g_toolbar;

/* ─── Tool button callbacks ──────────────────────────────────────────────── */

static void set_tool(SnapxAnnotationTool tool)
{
    g_toolbar.active_tool = tool;
    if (g_toolbar.canvas)
        snapx_canvas_set_tool(g_toolbar.canvas, tool);
}

static void on_tool_rect(GtkButton *b,      gpointer d) { (void)b;(void)d; set_tool(SNAPX_TOOL_RECT);      }
static void on_tool_arrow(GtkButton *b,     gpointer d) { (void)b;(void)d; set_tool(SNAPX_TOOL_ARROW);     }
static void on_tool_pen(GtkButton *b,       gpointer d) { (void)b;(void)d; set_tool(SNAPX_TOOL_PEN);       }
static void on_tool_text(GtkButton *b,      gpointer d) { (void)b;(void)d; set_tool(SNAPX_TOOL_TEXT);      }
static void on_tool_blur(GtkButton *b,      gpointer d) { (void)b;(void)d; set_tool(SNAPX_TOOL_BLUR);      }
static void on_tool_highlight(GtkButton *b, gpointer d) { (void)b;(void)d; set_tool(SNAPX_TOOL_HIGHLIGHT); }

static void on_color_notify(GObject *obj, GParamSpec *pspec, gpointer data)
{
    (void)pspec; (void)data;
#if GTK_CHECK_VERSION(4, 10, 0)
    const GdkRGBA *c = gtk_color_dialog_button_get_rgba(
        GTK_COLOR_DIALOG_BUTTON(obj));
    if (c) g_toolbar.active_color = *c;
#else
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(obj), &g_toolbar.active_color);
#endif
    if (g_toolbar.canvas)
        snapx_canvas_set_color(g_toolbar.canvas, &g_toolbar.active_color);
}

static void on_undo(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (g_toolbar.canvas)  snapx_canvas_undo(g_toolbar.canvas);
    if (g_toolbar.drawing_area) gtk_widget_queue_draw(g_toolbar.drawing_area);
}

static void on_redo(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (g_toolbar.canvas)  snapx_canvas_redo(g_toolbar.canvas);
    if (g_toolbar.drawing_area) gtk_widget_queue_draw(g_toolbar.drawing_area);
}

/* ─── Canvas mouse events ────────────────────────────────────────────────── */

#ifdef SNAPX_USE_GTK4

static void on_canvas_press(GtkGestureClick *g, int n, double x, double y, gpointer d)
{
    (void)g; (void)n; (void)d;
    if (!g_toolbar.canvas) return;
    g_toolbar.in_stroke    = TRUE;
    g_toolbar.stroke_start_x = x;
    g_toolbar.stroke_start_y = y;
    snapx_canvas_stroke_begin(g_toolbar.canvas, x, y);
}

static void on_canvas_release(GtkGestureClick *g, int n, double x, double y, gpointer d)
{
    (void)g; (void)n;
    if (!g_toolbar.canvas || !g_toolbar.in_stroke) return;
    g_toolbar.in_stroke = FALSE;
    snapx_canvas_stroke_end(g_toolbar.canvas, x, y);
    if (g_toolbar.drawing_area) gtk_widget_queue_draw(g_toolbar.drawing_area);
}

static void on_canvas_motion(GtkEventControllerMotion *ctrl, double x, double y, gpointer d)
{
    (void)ctrl; (void)d;
    if (!g_toolbar.canvas || !g_toolbar.in_stroke) return;
    snapx_canvas_stroke_update(g_toolbar.canvas, x, y);
    if (g_toolbar.drawing_area) gtk_widget_queue_draw(g_toolbar.drawing_area);
}

#else /* GTK3 */

static gboolean on_canvas_press_gtk3(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
    (void)w; (void)d;
    if (!g_toolbar.canvas || ev->button != 1) return FALSE;
    g_toolbar.in_stroke = TRUE;
    snapx_canvas_stroke_begin(g_toolbar.canvas, ev->x, ev->y);
    return FALSE;
}

static gboolean on_canvas_release_gtk3(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
    (void)w; (void)d;
    if (!g_toolbar.canvas || !g_toolbar.in_stroke || ev->button != 1) return FALSE;
    g_toolbar.in_stroke = FALSE;
    snapx_canvas_stroke_end(g_toolbar.canvas, ev->x, ev->y);
    if (g_toolbar.drawing_area) gtk_widget_queue_draw(g_toolbar.drawing_area);
    return FALSE;
}

static gboolean on_canvas_motion_gtk3(GtkWidget *w, GdkEventMotion *ev, gpointer d)
{
    (void)w; (void)d;
    if (!g_toolbar.canvas || !g_toolbar.in_stroke) return FALSE;
    snapx_canvas_stroke_update(g_toolbar.canvas, ev->x, ev->y);
    if (g_toolbar.drawing_area) gtk_widget_queue_draw(g_toolbar.drawing_area);
    return FALSE;
}

#endif

void snapx_toolbar_connect_canvas_events(GtkWidget *drawing_area,
                                          SnapxAnnotationCanvas *canvas)
{
    g_toolbar.canvas       = canvas;
    g_toolbar.drawing_area = drawing_area;

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
    g_signal_connect(drawing_area, "button-press-event",   G_CALLBACK(on_canvas_press_gtk3),   NULL);
    g_signal_connect(drawing_area, "button-release-event", G_CALLBACK(on_canvas_release_gtk3), NULL);
    g_signal_connect(drawing_area, "motion-notify-event",  G_CALLBACK(on_canvas_motion_gtk3),  NULL);
#endif
}

/* ─── Toolbar widget construction ────────────────────────────────────────── */

GtkWidget *snapx_toolbar_create(SnapxAnnotationCanvas *canvas,
                                 GtkWidget *drawing_area)
{
    g_toolbar.canvas       = canvas;
    g_toolbar.drawing_area = drawing_area;
    g_toolbar.active_tool  = SNAPX_TOOL_RECT;
    g_toolbar.active_color = (GdkRGBA){ 1.0, 0.2, 0.2, 1.0 };

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);
    gtk_widget_set_margin_top(bar, 4);
    gtk_widget_set_margin_bottom(bar, 4);

    /* Tool buttons */
    struct { const char *label; GCallback cb; } tools[] = {
        { "▭ Rect",  G_CALLBACK(on_tool_rect)      },
        { "→ Arrow", G_CALLBACK(on_tool_arrow)     },
        { "✏ Pen",   G_CALLBACK(on_tool_pen)       },
        { "T Text",  G_CALLBACK(on_tool_text)      },
        { "⬛ Blur",  G_CALLBACK(on_tool_blur)      },
        { "🖊 Hi",    G_CALLBACK(on_tool_highlight) },
    };

    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        GtkWidget *btn = gtk_button_new_with_label(tools[i].label);
        g_signal_connect(btn, "clicked", tools[i].cb, NULL);
#ifdef SNAPX_USE_GTK4
        gtk_box_append(GTK_BOX(bar), btn);
#else
        gtk_box_pack_start(GTK_BOX(bar), btn, FALSE, FALSE, 2);
#endif
    }

    /* Separator */
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
#ifdef SNAPX_USE_GTK4
    gtk_box_append(GTK_BOX(bar), sep);
#else
    gtk_box_pack_start(GTK_BOX(bar), sep, FALSE, FALSE, 4);
#endif

    /* Color button — GtkColorDialogButton (GTK 4.10+) or fallback */
    GtkWidget *color_btn;
#if GTK_CHECK_VERSION(4, 10, 0)
    GtkColorDialog *cdlg = gtk_color_dialog_new();
    gtk_color_dialog_set_title(cdlg, "Annotation Color");
    gtk_color_dialog_set_with_alpha(cdlg, FALSE);
    color_btn = gtk_color_dialog_button_new(cdlg);
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(color_btn),
                                      &g_toolbar.active_color);
    g_object_unref(cdlg);
    g_signal_connect(color_btn, "notify::rgba", G_CALLBACK(on_color_notify), NULL);
#else
    color_btn = gtk_color_button_new_with_rgba(&g_toolbar.active_color);
    gtk_color_button_set_title(GTK_COLOR_BUTTON(color_btn), "Annotation Color");
    g_signal_connect(color_btn, "color-set", G_CALLBACK(on_color_notify), NULL);
#endif
#ifdef SNAPX_USE_GTK4
    gtk_box_append(GTK_BOX(bar), color_btn);
#else
    gtk_box_pack_start(GTK_BOX(bar), color_btn, FALSE, FALSE, 2);
#endif

    /* Spacer */
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
#ifdef SNAPX_USE_GTK4
    gtk_box_append(GTK_BOX(bar), spacer);
#else
    gtk_box_pack_start(GTK_BOX(bar), spacer, TRUE, TRUE, 0);
#endif

    /* Undo / Redo */
    GtkWidget *btn_undo = gtk_button_new_with_label("↩ Undo");
    GtkWidget *btn_redo = gtk_button_new_with_label("↪ Redo");
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
