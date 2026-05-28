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

    /* In-progress stroke (not yet committed) */
    SnapxAnnotation *pending;

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

    /* Push onto undo stack */
    n->prev        = (AnnNode *)canvas->undo_head;
    n->ann.next    = NULL;
    canvas->undo_head = &n->ann;
    canvas->undo_depth++;
}

/* ─── Undo / Redo ────────────────────────────────────────────────────────── */

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

void snapx_canvas_render(const SnapxAnnotationCanvas *canvas, cairo_t *cr)
{
    if (!canvas || !cr) return;

    /* Collect annotations in order (undo_head is most recent; render oldest first) */
#define MAX_DEPTH 1024
    const SnapxAnnotation *stack[MAX_DEPTH];
    int depth = 0;
    const SnapxAnnotation *a = canvas->undo_head;
    while (a && depth < MAX_DEPTH) {
        stack[depth++] = a;
        a = &((AnnNode *)a)->prev->ann;  /* walk prev chain */
        /* Guard: prev of head node is NULL */
        if (!((AnnNode *)canvas->undo_head)[0].prev && depth > 0) {
            /* Reached bottom */
            break;
        }
        /* Safer traversal via AnnNode.prev */
        const AnnNode *cur_node = (const AnnNode *)stack[depth - 1];
        a = cur_node->prev ? &cur_node->prev->ann : NULL;
    }

    /* Render from oldest (bottom of stack) to newest */
    for (int i = depth - 1; i >= 0; i--)
        snapx_draw_annotation(cr, stack[i]);

    /* Render pending (in-progress) stroke with slight transparency */
    if (canvas->pending) {
        cairo_save(cr);
        cairo_push_group(cr);
        snapx_draw_annotation(cr, canvas->pending);
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, 0.8);
        cairo_restore(cr);
    }
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
