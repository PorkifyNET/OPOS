/*
 * The vector rasteriser of svg.c, shared with <canvas> (canvas.c): paths
 * flattened to polygons in device pixels, and an exact-area
 * anti-aliasing rasteriser that accumulates signed area per pixel row.
 */
#ifndef BROWSER_VECTOR_H
#define BROWSER_VECTOR_H

#include <stdbool.h>

/* x' = a x + c y + e, y' = b x + d y + f */
struct matrix {
    double a, b, c, d, e, f;
};

struct raster {
    int width, height;
    float *cover;         /* (width + 2) * height signed area deltas */
    float *pixels;        /* premultiplied RGBA, width * height * 4 (SVG) */
    int top, bottom;      /* rows that hold area deltas */
};

/* Points in device pixels; a subpath starts at starts[i]. */
struct path {
    double *xy;
    int count, capacity;
    int *starts;
    bool *closed;
    int subpaths, sub_capacity;
    bool failed;
    /* Bounding box in user units, for objectBoundingBox gradients. */
    double min_x, min_y, max_x, max_y;
    bool have_box;
};

/* Builds a path in user units, flattened through a matrix to device
   pixels. */
struct builder {
    struct path *path;
    struct matrix m;
    double x, y;           /* current point (user units) */
    double start_x, start_y;
    bool open;             /* a subpath has been started */
};

double vec_sqrt(double x);
double vec_sin(double x);
double vec_cos(double x);
double vec_atan2(double y, double x);
double vec_floor(double x);

/* The transform that applies n first, then m. */
struct matrix vec_multiply(struct matrix m, struct matrix n);
bool vec_invert(struct matrix m, struct matrix *out);
void vec_apply(const struct matrix *m, double x, double y, double *ox,
               double *oy);

/* Adds the signed area of one edge to the raster's rows. */
void vec_line(struct raster *r, double x0, double y0, double x1, double y1);

void vec_path_reset(struct path *path);
void vec_path_free(struct path *path);
int vec_subpath_end(const struct path *path, int index);

void vec_move(struct builder *b, double x, double y);
void vec_lineto(struct builder *b, double x, double y);
void vec_cubic(struct builder *b, double x1, double y1, double x2, double y2,
               double x3, double y3);
void vec_quad(struct builder *b, double x1, double y1, double x2, double y2);
void vec_close(struct builder *b);
/* Adds a point already in device pixels to the open subpath. */
void vec_point(struct builder *b, double device_x, double device_y);

/* Adds the outline of the path's stroke (half its width, in device
   pixels) to the raster as positive area. cap: 0 butt, 1 round,
   2 square; join: 0 miter, 1 round, 2 bevel. */
void vec_stroke(struct raster *r, const struct path *path, int cap, int join,
                double miter_limit, double half);

#endif
