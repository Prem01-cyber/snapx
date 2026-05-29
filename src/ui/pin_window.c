/**
 * @file pin_window.c
 * @brief Always-on-top pinned screenshot reference window.
 */

#include "pin_window.h"

#include <stdlib.h>
#include <gtk/gtk.h>
#include <cairo/cairo.h>

typedef struct {
    cairo_surface_t *surf;
} PinData;

static void pin_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
    (void)area;
    PinData *pd = data;
    if (!pd || !pd->surf) return;
    cairo_set_source_surface(cr, pd->surf, 0, 0);
    cairo_paint(cr);
}

static void pin_close(GtkWindow *win, gpointer data)
{
    PinData *pd = data;
    if (pd) {
        if (pd->surf) cairo_surface_destroy(pd->surf);
        free(pd);
    }
    gtk_window_destroy(win);
}

void snapx_pin_image(const SnapxImage *img)
{
    if (!img || !img->data) return;

    cairo_surface_t *surf = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, img->width, img->height);
    if (!surf) return;

    cairo_t *cr = cairo_create(surf);
    for (int y = 0; y < img->height; y++) {
        const uint8_t *s = img->data + y * img->stride;
        uint8_t *d = cairo_image_surface_get_data(surf)
                     + y * cairo_image_surface_get_stride(surf);
        for (int x = 0; x < img->width; x++) {
            d[x*4+0] = (uint8_t)(s[x*4+2] * s[x*4+3] / 255);
            d[x*4+1] = (uint8_t)(s[x*4+1] * s[x*4+3] / 255);
            d[x*4+2] = (uint8_t)(s[x*4+0] * s[x*4+3] / 255);
            d[x*4+3] = s[x*4+3];
        }
    }
    cairo_destroy(cr);
    cairo_surface_flush(surf);

    PinData *pd = calloc(1, sizeof(*pd));
    if (!pd) { cairo_surface_destroy(surf); return; }
    pd->surf = surf;

    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), "snapx — pinned");
    gtk_window_set_decorated(GTK_WINDOW(win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(win), img->width, img->height);

    GtkWidget *da = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), pin_draw, pd, NULL);
    gtk_window_set_child(GTK_WINDOW(win), da);

    g_signal_connect(win, "close-request", G_CALLBACK(pin_close), pd);
    gtk_widget_set_visible(win, TRUE);
}
