/**
 * @file canvas.c
 * @brief Annotation canvas management: undo/redo stack, stroke lifecycle, flatten.
 *
 * The canvas owns a doubly-linked list of SnapxAnnotation nodes.
 * Undo moves the head pointer back; redo restores it.
 * The redo branch is discarded when a new stroke begins.
 *
 * Stroke lifecycle:
 *   stroke_begin()  → allocates a pending annotation
 *   stroke_update() → appends points (PEN) or updates end-point (RECT/ARROW)
 *   stroke_end()    → commits pending to the undo stack
 */

#include "canvas.h"
#include "annotation.h"
#include "../capture/capture.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ─── Canvas struct ──────────────────────────────────────────────────────── */

struct SnapxAnnotationCanvas {
    int   image_width;
    int   image_height;

    SnapxAnnotationTool  active_tool;
    GdkRGBA              active_color;
    double               line_width;

    /* Undo stack (head = most recent committed annotation) */
    SnapxAnnotation *undo_head;  /**< Doubly-linked via ->next / ->prev       */
    int              undo_depth; /**< Number of committed annotations          */
    int              redo_depth;

    int              callout_next;

    /* In-progress stroke (not yet committed) */
    SnapxAnnotation *pending;

    SnapxBlurCommittedFn blur_fn;
    gpointer            blur_ud;

    /* Scale: canvas coords → image coords (set if preview is scaled) */
    double scale_x;
    double scale_y;
};

/* We embed a 'prev' pointer inside SnapxAnnotation via the 'next' field trick;
 * use a simple singly-linked list with a separate redo stack pointer instead. */
typedef struct AnnNode {
    SnapxAnnotation  ann;
    struct AnnNode  *prev;  /**< Previous node (undo direction)                */
} AnnNode;

/* ─── Allocation helpers ─────────────────────────────────────────────────── */

static AnnNode *annnode_alloc(SnapxAnnotationTool tool,
                               const GdkRGBA *color, double lw)
{
    AnnNode *n = calloc(1, sizeof(AnnNode));
    if (!n) return NULL;
    n->ann.tool       = tool;
    n->ann.color      = *color;
    n->ann.line_width = lw;
    n->ann.font_size  = 18.0;
    return n;
}

static void annnode_free(AnnNode *n)
{
    if (!n) return;
    free(n->ann.points.xs);
    free(n->ann.points.ys);
    free(n);
}

static void free_from(AnnNode *head)
{
    while (head) {
        AnnNode *next = (AnnNode *)head->ann.next;
        annnode_free(head);
        head = next;
    }
}

/* ─── Points dynamic array ───────────────────────────────────────────────── */

static int stroke_push(SnapxStrokePoints *sp, double x, double y)
{
    if (sp->n == sp->cap) {
        int new_cap = sp->cap ? sp->cap * 2 : 256;
        double *nx = realloc(sp->xs, (size_t)new_cap * sizeof(double));
        double *ny = realloc(sp->ys, (size_t)new_cap * sizeof(double));
        if (!nx || !ny) { free(nx); free(ny); return -1; }
        sp->xs = nx; sp->ys = ny; sp->cap = new_cap;
    }
    sp->xs[sp->n] = x; sp->ys[sp->n] = y; sp->n++;
    return 0;
}

/* ─── Canvas API ─────────────────────────────────────────────────────────── */

SnapxAnnotationCanvas *snapx_canvas_new(int width, int height)
{
    SnapxAnnotationCanvas *c = calloc(1, sizeof(SnapxAnnotationCanvas));
    if (!c) return NULL;
    c->image_width  = width;
    c->image_height = height;
    c->active_tool  = SNAPX_TOOL_RECT;
    c->active_color = (GdkRGBA){ 1.0, 0.2, 0.2, 1.0 };
    c->line_width   = 2.5;
    c->scale_x      = 1.0;
    c->scale_y      = 1.0;
    c->callout_next = 1;
    return c;
}

void snapx_canvas_free(SnapxAnnotationCanvas *canvas)
{
    if (!canvas) return;
    free_from((AnnNode *)canvas->undo_head);
    annnode_free((AnnNode *)canvas->pending);
    free(canvas);
}

void snapx_canvas_set_tool(SnapxAnnotationCanvas *canvas, SnapxAnnotationTool tool)
{
    if (canvas) canvas->active_tool = tool;
}

void snapx_canvas_set_color(SnapxAnnotationCanvas *canvas, const GdkRGBA *color)
{
    if (canvas && color) canvas->active_color = *color;
}

void snapx_canvas_set_blur_handler(SnapxAnnotationCanvas *canvas,
                                   SnapxBlurCommittedFn fn, gpointer userdata)
{
    if (!canvas) return;
    canvas->blur_fn = fn;
    canvas->blur_ud = userdata;
}

static void draw_blur_preview(cairo_t *cr, const SnapxAnnotation *ann)
{
    double x = ann->rect.x1 < ann->rect.x2 ? ann->rect.x1 : ann->rect.x2;
    double y = ann->rect.y1 < ann->rect.y2 ? ann->rect.y1 : ann->rect.y2;
    double w = fabs(ann->rect.x2 - ann->rect.x1);
    double h = fabs(ann->rect.y2 - ann->rect.y1);
    if (w < 1 || h < 1) return;

    cairo_save(cr);
    double dash[] = { 6.0, 4.0 };
    cairo_set_dash(cr, dash, 2, 0);
    cairo_set_source_rgba(cr, 0.25, 0.65, 1.0, 0.85);
    cairo_set_line_width(cr, 2.0);
    cairo_rectangle(cr, x, y, w, h);
    cairo_stroke(cr);
    cairo_restore(cr);
}

/* ─── Stroke lifecycle ───────────────────────────────────────────────────── */

void snapx_canvas_stroke_begin(SnapxAnnotationCanvas *canvas, double x, double y)
{
    if (!canvas) return;

    /* Discard any pending redo branch and create fresh pending */
    annnode_free((AnnNode *)canvas->pending);
    canvas->pending = NULL;
    canvas->redo_depth = 0;

    AnnNode *n = annnode_alloc(canvas->active_tool, &canvas->active_color,
                                canvas->line_width);
    if (!n) return;
    canvas->pending = &n->ann;

    n->ann.rect.x1 = x; n->ann.rect.y1 = y;
    n->ann.rect.x2 = x; n->ann.rect.y2 = y;

    if (canvas->active_tool == SNAPX_TOOL_PEN ||
        canvas->active_tool == SNAPX_TOOL_HIGHLIGHT) {
        stroke_push(&n->ann.points, x, y);
    }
}

void snapx_canvas_stroke_update(SnapxAnnotationCanvas *canvas, double x, double y)
{
    if (!canvas || !canvas->pending) return;
    AnnNode *n = (AnnNode *)canvas->pending;

    n->ann.rect.x2 = x;
    n->ann.rect.y2 = y;

    if (n->ann.tool == SNAPX_TOOL_PEN ||
        n->ann.tool == SNAPX_TOOL_HIGHLIGHT) {
        stroke_push(&n->ann.points, x, y);
    }
}

void snapx_canvas_stroke_end(SnapxAnnotationCanvas *canvas, double x, double y)
{
    if (!canvas || !canvas->pending) return;
    AnnNode *n = (AnnNode *)canvas->pending;
    canvas->pending = NULL;

    n->ann.rect.x2 = x;
    n->ann.rect.y2 = y;

    if (n->ann.tool == SNAPX_TOOL_PEN ||
        n->ann.tool == SNAPX_TOOL_HIGHLIGHT) {
        stroke_push(&n->ann.points, x, y);
    }

    if (n->ann.tool == SNAPX_TOOL_CALLOUT) {
        n->ann.callout_num = canvas->callout_next++;
        snprintf(n->ann.text, sizeof(n->ann.text), "%d", n->ann.callout_num);
    }

    if (n->ann.tool == SNAPX_TOOL_BLUR) {
        if (canvas->blur_fn)
            canvas->blur_fn(n->ann.rect.x1, n->ann.rect.y1,
                            n->ann.rect.x2, n->ann.rect.y2, canvas->blur_ud);
        annnode_free(n);
        return;
    }

    /* Push onto undo stack */
    n->prev        = (AnnNode *)canvas->undo_head;
    n->ann.next    = NULL;
    canvas->undo_head = &n->ann;
    canvas->undo_depth++;
}

void snapx_canvas_stroke_cancel(SnapxAnnotationCanvas *canvas)
{
    if (!canvas) return;
    annnode_free((AnnNode *)canvas->pending);
    canvas->pending = NULL;
}

void snapx_canvas_stroke_end_text(SnapxAnnotationCanvas *canvas,
                                  double x, double y, const char *text)
{
    if (!canvas || !canvas->pending) return;
    AnnNode *n = (AnnNode *)canvas->pending;
    if (text && text[0])
        snprintf(n->ann.text, sizeof(n->ann.text), "%s", text);
    else
        n->ann.text[0] = '\0';
    n->ann.rect.x2 = x;
    n->ann.rect.y2 = y;
    canvas->pending = NULL;
    n->prev        = (AnnNode *)canvas->undo_head;
    n->ann.next    = NULL;
    canvas->undo_head = &n->ann;
    canvas->undo_depth++;
}

void snapx_canvas_undo(SnapxAnnotationCanvas *canvas)
{
    if (!canvas || !canvas->undo_head) return;
    AnnNode *cur = (AnnNode *)canvas->undo_head;
    /* Move head to previous; detach cur and park it as pending (redo) */
    canvas->undo_head = cur->prev ? &cur->prev->ann : NULL;
    /* For simplicity we free redo'd annotations beyond 1 level — extend for full redo stack */
    annnode_free((AnnNode *)canvas->pending);
    cur->prev = NULL;
    canvas->pending    = &cur->ann;  /* temp redo slot */
    canvas->redo_depth = 1;
    canvas->undo_depth--;
}

void snapx_canvas_redo(SnapxAnnotationCanvas *canvas)
{
    if (!canvas || !canvas->pending || canvas->redo_depth == 0) return;
    AnnNode *n = (AnnNode *)canvas->pending;
    canvas->pending = NULL;
    canvas->redo_depth = 0;

    n->prev        = (AnnNode *)canvas->undo_head;
    n->ann.next    = NULL;
    canvas->undo_head = &n->ann;
    canvas->undo_depth++;
}

/* ─── Render ─────────────────────────────────────────────────────────────── */

#define MAX_ANNOT_DEPTH 1024

/**
 * Internal helper: walk the undo stack oldest-first into a local array,
 * render each annotation.  Does NOT render the pending stroke.
 */
static void render_committed_internal(const SnapxAnnotationCanvas *canvas,
                                       cairo_t *cr)
{
    const SnapxAnnotation *stack[MAX_ANNOT_DEPTH];
    int depth = 0;
    const SnapxAnnotation *a = canvas->undo_head;
    while (a && depth < MAX_ANNOT_DEPTH) {
        stack[depth++] = a;
        const AnnNode *node = (const AnnNode *)stack[depth - 1];
        a = node->prev ? &node->prev->ann : NULL;
    }
    for (int i = depth - 1; i >= 0; i--)
        snapx_draw_annotation(cr, stack[i]);
}

void snapx_canvas_render(const SnapxAnnotationCanvas *canvas, cairo_t *cr)
{
    if (!canvas || !cr) return;
    render_committed_internal(canvas, cr);

    /* Render pending (in-progress) stroke at 80% opacity */
    if (canvas->pending) {
        cairo_save(cr);
        cairo_push_group(cr);
        snapx_draw_annotation(cr, canvas->pending);
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, 0.8);
        cairo_restore(cr);
    }
}

void snapx_canvas_render_committed(const SnapxAnnotationCanvas *canvas, cairo_t *cr)
{
    if (!canvas || !cr) return;
    render_committed_internal(canvas, cr);
}

void snapx_canvas_render_pending(const SnapxAnnotationCanvas *canvas, cairo_t *cr)
{
    if (!canvas || !cr || !canvas->pending) return;

    SnapxAnnotationTool tool = canvas->pending->tool;
    if (tool == SNAPX_TOOL_BLUR) {
        draw_blur_preview(cr, canvas->pending);
        return;
    }
    if (tool == SNAPX_TOOL_RECT || tool == SNAPX_TOOL_ARROW ||
        tool == SNAPX_TOOL_HIGHLIGHT || tool == SNAPX_TOOL_REDACT) {
        snapx_draw_annotation(cr, canvas->pending);
        return;
    }

    /* Pen / text preview: soft alpha via off-screen group */
    cairo_save(cr);
    cairo_push_group(cr);
    snapx_draw_annotation(cr, canvas->pending);
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, 0.8);
    cairo_restore(cr);
}

int snapx_canvas_has_pending(const SnapxAnnotationCanvas *canvas)
{
    return canvas && canvas->pending ? 1 : 0;
}

/* ─── Flatten ────────────────────────────────────────────────────────────── */

SnapxImage *snapx_canvas_flatten(const SnapxAnnotationCanvas *canvas,
                                  const SnapxImage *base)
{
    if (!base) return NULL;

    SnapxImage *flat = snapx_image_alloc(base->width, base->height);
    if (!flat) return NULL;
    memcpy(flat->data, base->data, (size_t)(base->stride * base->height));
    flat->scale = base->scale;

    if (!canvas) return flat;

    /* Create a Cairo surface wrapping the flat image */
    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        flat->data, CAIRO_FORMAT_ARGB32,
        flat->width, flat->height, flat->stride);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return flat;
    }

    cairo_t *cr = cairo_create(surf);
    snapx_canvas_render(canvas, cr);
    cairo_destroy(cr);
    cairo_surface_flush(surf);
    cairo_surface_destroy(surf);

    return flat;
}
