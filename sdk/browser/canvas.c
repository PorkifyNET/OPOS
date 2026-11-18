/*
 * <canvas> 2D drawing for scripts. Each canvas element gets a surface
 * (straight-alpha 0xAARRGGBB pixels) and a drawing state when a script
 * first draws on it. dom.js's CanvasRenderingContext2D calls one native,
 * N.cv(node, op, ...), with the operations below; the browser shows the
 * surface as the element's image (image_ref_scaled).
 *
 * Paths, strokes and anti-aliasing come from svg.c's rasteriser
 * (vector.h). Colours, gradients (linear, radial with two circles, conic)
 * and patterns are painted with the Porter-Duff operators of
 * globalCompositeOperation (blend modes draw as source-over), through
 * the clip, at globalAlpha. Text uses the browser's fonts; transforms
 * move and scale text but do not rotate it. Shadows and filters are not
 * drawn.
 *
 * Images drawn from another site taint the canvas: reading its pixels
 * (getImageData, toDataURL) then fails, as in other browsers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opos_font.h>
#include <opos_http.h>

#include "browser.h"
#include "canvas.h"
#include "vector.h"

#define CANVAS_MAX_PIXELS (16u * 1024 * 1024)
#define CANVAS_STACK 32
#define CANVAS_STOPS 32
#define CANVAS_DASHES 16
#define CANVAS_PI 3.14159265358979323846
#define IMAGE_MAX_BYTES (32 << 20)

/* The operations of N.cv (the same numbers are in dom.js). */
enum {
    OP_SIZE, OP_RESET, OP_SAVE, OP_RESTORE, OP_TRANSFORM, OP_SET_TRANSFORM,
    OP_GET_TRANSFORM, OP_BEGIN_PATH, OP_MOVE, OP_LINE, OP_QUAD, OP_CUBIC,
    OP_ARC, OP_ARC_TO, OP_ELLIPSE, OP_RECT, OP_CLOSE, OP_FILL, OP_STROKE,
    OP_CLIP, OP_FILL_RECT, OP_STROKE_RECT, OP_CLEAR_RECT, OP_STYLE,
    OP_GRADIENT, OP_PATTERN, OP_LINE_WIDTH, OP_CAP, OP_JOIN, OP_MITER,
    OP_ALPHA, OP_COMPOSITE, OP_FONT, OP_ALIGN, OP_BASELINE, OP_DASH,
    OP_DASH_OFFSET, OP_SMOOTHING, OP_FILL_TEXT, OP_STROKE_TEXT, OP_MEASURE,
    OP_DRAW_IMAGE, OP_GET_IMAGE_DATA, OP_PUT_IMAGE_DATA, OP_TO_DATA_URL,
    OP_IN_PATH, OP_IN_STROKE, OP_IMAGE_SIZE, OP_LOAD_IMAGE
};

/* globalCompositeOperation, in dom.js's order. */
enum {
    CO_SOURCE_OVER, CO_SOURCE_IN, CO_SOURCE_OUT, CO_SOURCE_ATOP,
    CO_DESTINATION_OVER, CO_DESTINATION_IN, CO_DESTINATION_OUT,
    CO_DESTINATION_ATOP, CO_LIGHTER, CO_COPY, CO_XOR
};

enum { PAINT_COLOUR, PAINT_LINEAR, PAINT_RADIAL, PAINT_CONIC, PAINT_PATTERN,
       PAINT_IMAGE };

/* A pattern's own copy of its image (shared by saved states). */
struct pattern {
    int refs;
    uint32_t *pixels;
    int width, height;
    bool repeat_x, repeat_y;
};

struct clip {
    int refs;
    float *cover;          /* width * height, 0..1 */
};

struct paint {
    int kind;
    float rgba[4];         /* colour, premultiplied */
    double x0, y0, r0, x1, y1, r1, angle;
    int stop_count;
    float offset[CANVAS_STOPS];
    float stop[CANVAS_STOPS][4];  /* premultiplied */
    struct pattern *pattern;
    /* PAINT_IMAGE (drawImage, only while it draws) */
    const uint32_t *pixels;
    int width, height;
    double sx, sy, sw, sh, dx, dy, dw, dh;
};

struct state {
    struct matrix m;
    struct paint fill, stroke;
    float alpha;
    int composite;
    double line_width, miter_limit;
    int cap, join;
    double dash[CANVAS_DASHES];
    int dash_count;
    double dash_offset;
    int font_size, font_index;
    int align;             /* 0 start, 1 end, 2 left, 3 right, 4 center */
    int baseline;          /* 0 alphabetic, 1 top, 2 hanging, 3 middle,
                              4 ideographic, 5 bottom */
    bool smoothing;
    struct clip *clip;
};

struct canvas {
    struct canvas *next;
    struct page *page;
    const struct opos_html_node *node;
    int width, height;
    uint32_t *pixels;
    unsigned version;
    bool tainted;
    struct state st;
    struct state stack[CANVAS_STACK];
    int depth;
    struct path path;      /* the current path, in device pixels */
    struct builder build;
    bool have_point;
    struct path scratch;   /* rectangles, dashes */
    struct raster raster;
};

/* An image a script loaded (new Image(), src set before it is in the
   page). */
struct loaded {
    struct loaded *next;
    struct page *page;
    const struct opos_html_node *node;
    struct image_ref *ref;
    bool foreign;
};

static struct canvas *canvases;
static struct loaded *loaded_images;
static bool painted;

/* ------------------------------------------------------------ helpers */

static double absolute(double x) { return x < 0 ? -x : x; }

static double modulo(double x, double y) {
    return x - vec_floor(x / y) * y;
}

static int attribute_number(const struct opos_html_node *node,
                            const char *name, int fallback) {
    const char *text = opos_html_attribute(node, name);
    if (text == NULL) {
        return fallback;
    }
    while (*text == ' ' || *text == '\t' || *text == '\n') {
        text++;
    }
    if (*text < '0' || *text > '9') {
        return fallback;
    }
    long value = 0;
    while (*text >= '0' && *text <= '9' && value < 100000) {
        value = value * 10 + (*text++ - '0');
    }
    return (int)value;
}

void canvas_size(const struct opos_html_node *node, int *width,
                 int *height) {
    *width = attribute_number(node, "width", 300);
    *height = attribute_number(node, "height", 150);
}

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

static void colour_rgba(uint32_t colour, int alpha, float out[4]) {
    float a = alpha / 255.0f;
    out[0] = ((colour >> 16) & 255) / 255.0f * a;
    out[1] = ((colour >> 8) & 255) / 255.0f * a;
    out[2] = (colour & 255) / 255.0f * a;
    out[3] = a;
}

/* 0xAARRGGBB to premultiplied floats, and back. */
static void unpack(uint32_t p, float out[4]) {
    float a = (p >> 24) / 255.0f;
    out[0] = ((p >> 16) & 255) / 255.0f * a;
    out[1] = ((p >> 8) & 255) / 255.0f * a;
    out[2] = (p & 255) / 255.0f * a;
    out[3] = a;
}

static uint32_t pack(const float in[4]) {
    float a = clamp01(in[3]);
    if (a <= 0.5f / 255) {
        return 0;
    }
    uint32_t r = (uint32_t)(clamp01(in[0] / a) * 255 + 0.5f);
    uint32_t g = (uint32_t)(clamp01(in[1] / a) * 255 + 0.5f);
    uint32_t b = (uint32_t)(clamp01(in[2] / a) * 255 + 0.5f);
    return (uint32_t)(a * 255 + 0.5f) << 24 | r << 16 | g << 8 | b;
}

static void pattern_release(struct pattern *p) {
    if (p != NULL && --p->refs == 0) {
        free(p->pixels);
        free(p);
    }
}

static void clip_release(struct clip *c) {
    if (c != NULL && --c->refs == 0) {
        free(c->cover);
        free(c);
    }
}

/* Takes references for a state copy. */
static void state_hold(struct state *s) {
    if (s->fill.pattern != NULL) s->fill.pattern->refs++;
    if (s->stroke.pattern != NULL) s->stroke.pattern->refs++;
    if (s->clip != NULL) s->clip->refs++;
}

static void state_drop(struct state *s) {
    pattern_release(s->fill.pattern);
    pattern_release(s->stroke.pattern);
    clip_release(s->clip);
    s->fill.pattern = s->stroke.pattern = NULL;
    s->clip = NULL;
}

static void state_default(struct state *s) {
    memset(s, 0, sizeof(*s));
    s->m = (struct matrix){1, 0, 0, 1, 0, 0};
    s->fill.kind = s->stroke.kind = PAINT_COLOUR;
    s->fill.rgba[3] = s->stroke.rgba[3] = 1;
    s->alpha = 1;
    s->line_width = 1;
    s->miter_limit = 10;
    s->font_size = 10;
    s->smoothing = true;
}

static void canvas_clear_state(struct canvas *c) {
    while (c->depth > 0) {
        state_drop(&c->stack[--c->depth]);
    }
    state_drop(&c->st);
    state_default(&c->st);
    vec_path_reset(&c->path);
    c->build.open = false;
    c->have_point = false;
}

/* Gives the canvas the size of its attributes; a new size clears it. */
static bool canvas_fit(struct canvas *c) {
    int width, height;
    canvas_size(c->node, &width, &height);
    if (width == c->width && height == c->height
        && (c->pixels != NULL || width == 0 || height == 0)) {
        return c->pixels != NULL;
    }
    free(c->pixels);
    free(c->raster.cover);
    c->pixels = NULL;
    c->raster.cover = NULL;
    c->width = width;
    c->height = height;
    canvas_clear_state(c);
    c->version++;
    painted = true;
    if (width <= 0 || height <= 0
        || (size_t)width * (size_t)height > CANVAS_MAX_PIXELS) {
        return false;
    }
    c->pixels = calloc((size_t)width * height, 4);
    c->raster.cover = calloc((size_t)(width + 2) * height, sizeof(float));
    if (c->pixels == NULL || c->raster.cover == NULL) {
        free(c->pixels);
        free(c->raster.cover);
        c->pixels = NULL;
        c->raster.cover = NULL;
        return false;
    }
    c->raster.width = width;
    c->raster.height = height;
    c->raster.top = height;
    c->raster.bottom = 0;
    return true;
}

static struct canvas *canvas_find(const struct opos_html_node *node) {
    for (struct canvas *c = canvases; c != NULL; c = c->next) {
        if (c->node == node) {
            return c;
        }
    }
    return NULL;
}

static struct canvas *canvas_get(struct page *page,
                                 const struct opos_html_node *node) {
    struct canvas *c = canvas_find(node);
    if (c == NULL) {
        c = calloc(1, sizeof(*c));
        if (c == NULL) {
            return NULL;
        }
        c->page = page;
        c->node = node;
        c->width = c->height = -1;
        state_default(&c->st);
        c->next = canvases;
        canvases = c;
    }
    canvas_fit(c);
    return c;
}

const uint32_t *canvas_pixels(const struct opos_html_node *node, int *width,
                              int *height, unsigned *version) {
    struct canvas *c = canvas_find(node);
    if (c == NULL || c->pixels == NULL) {
        return NULL;
    }
    *width = c->width;
    *height = c->height;
    *version = c->version;
    return c->pixels;
}

bool canvas_take_painted(void) {
    bool was = painted;
    painted = false;
    return was;
}

static void canvas_free(struct canvas *c) {
    canvas_clear_state(c);
    vec_path_free(&c->path);
    vec_path_free(&c->scratch);
    free(c->pixels);
    free(c->raster.cover);
    free(c);
}

void canvas_page_end(struct page *page) {
    for (struct canvas **p = &canvases; *p != NULL;) {
        struct canvas *c = *p;
        if (c->page == page) {
            *p = c->next;
            canvas_free(c);
        } else {
            p = &c->next;
        }
    }
    for (struct loaded **p = &loaded_images; *p != NULL;) {
        struct loaded *l = *p;
        if (l->page == page) {
            *p = l->next;
            image_ref_free(l->ref);
            free(l);
        } else {
            p = &l->next;
        }
    }
}

/* ---------------------------------------------------------- images */

/* The pixels of an <img> (loaded by a script or by the page) or of a
   <canvas>, at its natural size. */
static const uint32_t *source_pixels(struct page *page,
                                     const struct opos_html_node *node,
                                     int *width, int *height,
                                     bool *foreign) {
    *foreign = false;
    if (node->tag == HTML_TAG_CANVAS) {
        struct canvas *c = canvas_find(node);
        if (c == NULL || c->pixels == NULL) {
            return NULL;
        }
        *width = c->width;
        *height = c->height;
        *foreign = c->tainted;
        return c->pixels;
    }
    struct image_ref *ref = NULL;
    for (struct loaded *l = loaded_images; l != NULL; l = l->next) {
        if (l->node == node && l->page == page) {
            ref = l->ref;
            *foreign = l->foreign;
            break;
        }
    }
    if (ref == NULL) {
        for (size_t i = 0; i < page->image_count; i++) {
            if (page->images[i]->node == node) {
                ref = page->images[i];
                *foreign = ref->data_url == NULL
                        && !script_same_origin(page->url, ref->url);
                break;
            }
        }
    }
    if (ref == NULL || ref->state != 1 || ref->width <= 0
        || ref->height <= 0) {
        return NULL;
    }
    const uint32_t *pixels = image_ref_scaled(ref, ref->width, ref->height);
    if (pixels == NULL) {
        return NULL;
    }
    *width = ref->width;
    *height = ref->height;
    return pixels;
}

static struct image_ref *load_image(struct page *page, const char *url,
                                    bool *foreign) {
    struct image_ref *ref = calloc(1, sizeof(*ref));
    if (ref == NULL) {
        return NULL;
    }
    *foreign = false;
    if (strncmp(url, "data:", 5) == 0) {
        size_t length;
        uint8_t *bytes = data_url_decode(url, &length);
        if (bytes != NULL) {
            image_ref_decode(ref, bytes, length);
            free(bytes);
        }
    } else {
        bool same = script_same_origin(page->url, url);
        *foreign = !same;
        char status[OPOS_HTTP_URL_MAX + 16];
        snprintf(status, sizeof(status), "Loading %s", url);
        browser_status(status);
        struct opos_http_response response;
        if (same) {
            cookies_attach(true);
        }
        long error = opos_http_request("GET", url, "", NULL, NULL, 0,
                                       IMAGE_MAX_BYTES, &response);
        if (same) {
            cookies_detach();
        }
        if (error == 0) {
            if (response.status >= 200 && response.status < 300) {
                image_ref_decode(ref, response.body, response.length);
            }
            opos_http_free(&response);
        }
        browser_status("");
    }
    if (ref->state != 1) {
        image_ref_free(ref);
        return NULL;
    }
    return ref;
}

/* ------------------------------------------------------------- paint */

static void stop_colour(const struct paint *p, double t, float out[4]) {
    int last = p->stop_count - 1;
    if (t <= p->offset[0]) {
        memcpy(out, p->stop[0], sizeof(float) * 4);
        return;
    }
    if (t >= p->offset[last]) {
        memcpy(out, p->stop[last], sizeof(float) * 4);
        return;
    }
    for (int i = 0; i < last; i++) {
        if (t <= p->offset[i + 1]) {
            float span = p->offset[i + 1] - p->offset[i];
            float f = span > 0 ? (float)((t - p->offset[i]) / span) : 1;
            for (int k = 0; k < 4; k++) {
                out[k] = p->stop[i][k] + (p->stop[i + 1][k] - p->stop[i][k]) * f;
            }
            return;
        }
    }
    memcpy(out, p->stop[last], sizeof(float) * 4);
}

static uint32_t pixel_at(const uint32_t *pixels, int width, int height,
                         int x, int y) {
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return 0;
    }
    return pixels[(size_t)y * width + x];
}

/* Samples an image at (u, v) in its pixels (centres at +0.5). */
static void sample(const uint32_t *pixels, int width, int height, double u,
                   double v, bool smooth, int wrap_x, int wrap_y,
                   float out[4]) {
    if (!smooth) {
        int x = (int)vec_floor(u), y = (int)vec_floor(v);
        if (wrap_x) x = (int)modulo(x, width);
        if (wrap_y) y = (int)modulo(y, height);
        unpack(pixel_at(pixels, width, height, x, y), out);
        return;
    }
    u -= 0.5;
    v -= 0.5;
    int x0 = (int)vec_floor(u), y0 = (int)vec_floor(v);
    float fx = (float)(u - x0), fy = (float)(v - y0);
    float corner[4][4];
    for (int k = 0; k < 4; k++) {
        int x = x0 + (k & 1), y = y0 + (k >> 1);
        if (wrap_x) {
            x = (int)modulo(x, width);
        } else if (wrap_x == 0) {
            x = x < 0 ? 0 : x >= width ? width - 1 : x; /* edges stay */
        }
        if (wrap_y) {
            y = (int)modulo(y, height);
        } else {
            y = y < 0 ? 0 : y >= height ? height - 1 : y;
        }
        unpack(pixel_at(pixels, width, height, x, y), corner[k]);
    }
    for (int i = 0; i < 4; i++) {
        float top = corner[0][i] + (corner[1][i] - corner[0][i]) * fx;
        float bottom = corner[2][i] + (corner[3][i] - corner[2][i]) * fx;
        out[i] = top + (bottom - top) * fy;
    }
}

/* The paint's premultiplied colour at device pixel centre (x, y);
   inverse maps device pixels to the paint's user space. */
static void paint_at(const struct paint *p, const struct matrix *inverse,
                     bool smooth, double x, double y, float out[4]) {
    if (p->kind == PAINT_COLOUR) {
        memcpy(out, p->rgba, sizeof(float) * 4);
        return;
    }
    double ux, uy;
    vec_apply(inverse, x, y, &ux, &uy);
    if (p->kind == PAINT_LINEAR) {
        double vx = p->x1 - p->x0, vy = p->y1 - p->y0;
        double length = vx * vx + vy * vy;
        if (length <= 0) {
            memset(out, 0, sizeof(float) * 4);
            return;
        }
        stop_colour(p, ((ux - p->x0) * vx + (uy - p->y0) * vy) / length, out);
    } else if (p->kind == PAINT_RADIAL) {
        /* The largest t with the point on the circle between the two. */
        double cdx = p->x1 - p->x0, cdy = p->y1 - p->y0, dr = p->r1 - p->r0;
        double pdx = ux - p->x0, pdy = uy - p->y0;
        double a = cdx * cdx + cdy * cdy - dr * dr;
        double b = pdx * cdx + pdy * cdy + p->r0 * dr;
        double c = pdx * pdx + pdy * pdy - p->r0 * p->r0;
        double t;
        bool found = false;
        if (absolute(a) < 1e-9) {
            if (absolute(b) > 1e-12) {
                t = c / (2 * b);
                found = p->r0 + t * dr >= 0;
            }
        } else {
            double disc = b * b - a * c;
            if (disc >= 0) {
                double root = vec_sqrt(disc);
                double t1 = (b + root) / a, t2 = (b - root) / a;
                double high = t1 > t2 ? t1 : t2, low = t1 > t2 ? t2 : t1;
                if (p->r0 + high * dr >= 0) {
                    t = high;
                    found = true;
                } else if (p->r0 + low * dr >= 0) {
                    t = low;
                    found = true;
                }
            }
        }
        if (!found) {
            memset(out, 0, sizeof(float) * 4);
            return;
        }
        stop_colour(p, t, out);
    } else if (p->kind == PAINT_CONIC) {
        double angle = vec_atan2(uy - p->y0, ux - p->x0) - p->angle;
        stop_colour(p, modulo(angle, 2 * CANVAS_PI) / (2 * CANVAS_PI), out);
    } else if (p->kind == PAINT_PATTERN) {
        const struct pattern *pt = p->pattern;
        if ((!pt->repeat_x && (ux < 0 || ux >= pt->width))
            || (!pt->repeat_y && (uy < 0 || uy >= pt->height))) {
            memset(out, 0, sizeof(float) * 4);
            return;
        }
        sample(pt->pixels, pt->width, pt->height, ux, uy, smooth,
               pt->repeat_x, pt->repeat_y, out);
    } else {
        double u = p->sx + (ux - p->dx) * p->sw / p->dw;
        double v = p->sy + (uy - p->dy) * p->sh / p->dh;
        sample(p->pixels, p->width, p->height, u, v, smooth, 0, 0, out);
    }
}

/* ------------------------------------------------------- compositing */

static bool unbounded(int op) {
    return op == CO_SOURCE_IN || op == CO_SOURCE_OUT
        || op == CO_DESTINATION_IN || op == CO_DESTINATION_ATOP
        || op == CO_COPY;
}

/* Paints the area accumulated in the raster (coverage clamped to 1, or
   even-odd) with paint, then clears the accumulation. */
static void composite(struct canvas *c, const struct paint *paint,
                      bool even_odd, float alpha) {
    struct raster *ra = &c->raster;
    const struct state *st = &c->st;
    int op = st->composite;
    bool all = unbounded(op);
    int stride = c->width + 2;
    int first = all ? 0 : ra->top, last = all ? c->height : ra->bottom;
    struct matrix inverse = {1, 0, 0, 1, 0, 0};
    if (paint->kind != PAINT_COLOUR && !vec_invert(st->m, &inverse)) {
        alpha = 0;
    }
    if (paint->kind == PAINT_IMAGE && (paint->dw == 0 || paint->dh == 0)) {
        alpha = 0;
    }
    alpha *= st->alpha;
    const float *clip = st->clip != NULL ? st->clip->cover : NULL;
    for (int y = first; y < last; y++) {
        float *row = ra->cover + (size_t)y * stride;
        uint32_t *line = c->pixels + (size_t)y * c->width;
        float sum = 0;
        bool touched = y >= ra->top && y < ra->bottom;
        for (int x = 0; x < c->width; x++) {
            float cover = 0;
            if (touched) {
                sum += row[x];
                row[x] = 0;
                cover = sum < 0 ? -sum : sum;
                if (even_odd) {
                    cover -= 2 * (float)(int)(cover / 2);
                    if (cover > 1) {
                        cover = 2 - cover;
                    }
                } else if (cover > 1) {
                    cover = 1;
                }
            }
            if (!all && cover < 1.0f / 512) {
                continue;
            }
            float k = clip != NULL ? clip[(size_t)y * c->width + x] : 1;
            if (k <= 0) {
                continue;
            }
            float s[4] = {0, 0, 0, 0};
            float f = cover * alpha;
            if (f > 0) {
                paint_at(paint, &inverse, st->smoothing, x + 0.5, y + 0.5, s);
                for (int i = 0; i < 4; i++) {
                    s[i] *= f;
                }
            }
            float d[4];
            unpack(line[x], d);
            float as = s[3], ad = d[3], fa, fb;
            switch (op) {
            case CO_SOURCE_IN: fa = ad; fb = 0; break;
            case CO_SOURCE_OUT: fa = 1 - ad; fb = 0; break;
            case CO_SOURCE_ATOP: fa = ad; fb = 1 - as; break;
            case CO_DESTINATION_OVER: fa = 1 - ad; fb = 1; break;
            case CO_DESTINATION_IN: fa = 0; fb = as; break;
            case CO_DESTINATION_OUT: fa = 0; fb = 1 - as; break;
            case CO_DESTINATION_ATOP: fa = 1 - ad; fb = as; break;
            case CO_LIGHTER: fa = 1; fb = 1; break;
            case CO_COPY: fa = 1; fb = 0; break;
            case CO_XOR: fa = 1 - ad; fb = 1 - as; break;
            default: fa = 1; fb = 1 - as; break;
            }
            float r[4];
            for (int i = 0; i < 4; i++) {
                r[i] = fa * s[i] + fb * d[i];
                if (r[i] > 1) {
                    r[i] = 1;
                }
                if (k < 1) {
                    r[i] = k * r[i] + (1 - k) * d[i];
                }
            }
            line[x] = pack(r);
        }
        if (touched) {
            row[c->width] = 0;
            row[c->width + 1] = 0;
        }
    }
    ra->top = c->height;
    ra->bottom = 0;
    c->version++;
    painted = true;
}

/* Adds a path's edges (every subpath closed) to the raster. */
static void raster_path(struct raster *ra, const struct path *path) {
    if (path->failed) {
        return;
    }
    for (int sp = 0; sp < path->subpaths; sp++) {
        int start = path->starts[sp], end = vec_subpath_end(path, sp);
        for (int i = start; i < end; i++) {
            int j = i + 1 < end ? i + 1 : start;
            vec_line(ra, path->xy[2 * i], path->xy[2 * i + 1],
                     path->xy[2 * j], path->xy[2 * j + 1]);
        }
    }
}

/* The raster's accumulated coverage as a w*h mask (clears it). */
static float *take_cover(struct canvas *c, bool even_odd) {
    struct raster *ra = &c->raster;
    float *mask = calloc((size_t)c->width * c->height, sizeof(float));
    int stride = c->width + 2;
    for (int y = ra->top; y < ra->bottom; y++) {
        float *row = ra->cover + (size_t)y * stride;
        float sum = 0;
        for (int x = 0; x < c->width; x++) {
            sum += row[x];
            row[x] = 0;
            float cover = sum < 0 ? -sum : sum;
            if (even_odd) {
                cover -= 2 * (float)(int)(cover / 2);
                if (cover > 1) {
                    cover = 2 - cover;
                }
            } else if (cover > 1) {
                cover = 1;
            }
            if (mask != NULL) {
                mask[(size_t)y * c->width + x] = cover;
            }
        }
        row[c->width] = 0;
        row[c->width + 1] = 0;
    }
    ra->top = c->height;
    ra->bottom = 0;
    return mask;
}

/* ------------------------------------------------------------- paths */

static void builder_ready(struct canvas *c, struct path *path) {
    c->build.path = path;
    c->build.m = c->st.m;
}

static void path_move_to(struct canvas *c, double x, double y) {
    builder_ready(c, &c->path);
    vec_move(&c->build, x, y);
    c->have_point = true;
}

static void path_line_to(struct canvas *c, double x, double y) {
    if (!c->have_point) {
        path_move_to(c, x, y);
        return;
    }
    builder_ready(c, &c->path);
    vec_lineto(&c->build, x, y);
}

/* Lines along an ellipse from angle a0 through sweep (radians). */
static void arc_points(struct canvas *c, double cx, double cy, double rx,
                       double ry, double rotation, double a0, double sweep) {
    double cr = vec_cos(rotation), sr = vec_sin(rotation);
    double scale = vec_sqrt(absolute(c->st.m.a * c->st.m.d
                                     - c->st.m.b * c->st.m.c));
    double length = absolute(sweep) * (rx > ry ? rx : ry) * scale;
    int n = (int)(vec_sqrt(length) * 2) + 4;
    if (n > 400) {
        n = 400;
    }
    for (int i = 0; i <= n; i++) {
        double t = a0 + sweep * i / n;
        double ex = rx * vec_cos(t), ey = ry * vec_sin(t);
        double px = cx + ex * cr - ey * sr, py = cy + ex * sr + ey * cr;
        if (i == 0) {
            path_line_to(c, px, py);
        } else {
            vec_lineto(&c->build, px, py);
        }
    }
}

static double arc_sweep(double a0, double a1, bool ccw) {
    double sweep = a1 - a0;
    double full = 2 * CANVAS_PI;
    if (!ccw) {
        if (sweep >= full) {
            return full;
        }
        sweep = modulo(sweep, full);
    } else {
        if (-sweep >= full) {
            return -full;
        }
        sweep = modulo(sweep, full);
        if (sweep > 0) {
            sweep -= full;
        }
    }
    return sweep;
}

static void path_arc_to(struct canvas *c, double x1, double y1, double x2,
                        double y2, double r) {
    if (!c->have_point) {
        path_move_to(c, x1, y1);
    }
    double x0 = c->build.x, y0 = c->build.y;
    double v1x = x0 - x1, v1y = y0 - y1, v2x = x2 - x1, v2y = y2 - y1;
    double l1 = vec_sqrt(v1x * v1x + v1y * v1y);
    double l2 = vec_sqrt(v2x * v2x + v2y * v2y);
    double cross = v1x * v2y - v1y * v2x;
    if (l1 < 1e-9 || l2 < 1e-9 || r <= 0 || absolute(cross) < 1e-9 * l1 * l2) {
        path_line_to(c, x1, y1);
        return;
    }
    v1x /= l1; v1y /= l1; v2x /= l2; v2y /= l2;
    double cosine = v1x * v2x + v1y * v2y;
    double distance = r * vec_sqrt((1 + cosine) / (1 - cosine));
    double t1x = x1 + v1x * distance, t1y = y1 + v1y * distance;
    double t2x = x1 + v2x * distance, t2y = y1 + v2y * distance;
    double bx = v1x + v2x, by = v1y + v2y;
    double bl = vec_sqrt(bx * bx + by * by);
    double centre_distance = r / vec_sqrt((1 - cosine) / 2);
    double cx = x1 + bx / bl * centre_distance;
    double cy = y1 + by / bl * centre_distance;
    double a0 = vec_atan2(t1y - cy, t1x - cx);
    double a1 = vec_atan2(t2y - cy, t2x - cx);
    double sweep = modulo(a1 - a0 + CANVAS_PI, 2 * CANVAS_PI) - CANVAS_PI;
    path_line_to(c, t1x, t1y);
    arc_points(c, cx, cy, r, r, 0, a0, sweep);
}

static void scratch_rect(struct canvas *c, double x, double y, double w,
                         double h) {
    vec_path_reset(&c->scratch);
    struct builder b = {.path = &c->scratch, .m = c->st.m};
    vec_move(&b, x, y);
    vec_lineto(&b, x + w, y);
    vec_lineto(&b, x + w, y + h);
    vec_lineto(&b, x, y + h);
    vec_close(&b);
}

/* --------------------------------------------------------------- stroke */

static double device_scale(const struct canvas *c) {
    return vec_sqrt(absolute(c->st.m.a * c->st.m.d - c->st.m.b * c->st.m.c));
}

/* The dashed pieces of path (device pixels) as open subpaths. */
static void dash_path(struct canvas *c, const struct path *path,
                      struct path *out) {
    const struct state *st = &c->st;
    double scale = device_scale(c);
    double total = 0;
    for (int i = 0; i < st->dash_count; i++) {
        total += st->dash[i];
    }
    vec_path_reset(out);
    struct builder b = {.path = out, .m = {1, 0, 0, 1, 0, 0}};
    if (total <= 0) {
        return;
    }
    for (int sp = 0; sp < path->subpaths; sp++) {
        int start = path->starts[sp], end = vec_subpath_end(path, sp);
        int n = end - start;
        if (n < 2) {
            continue;
        }
        /* Where the pattern starts. */
        double phase = modulo(st->dash_offset, total);
        int index = 0;
        while (phase >= st->dash[index] && st->dash_count > 0) {
            phase -= st->dash[index];
            index = (index + 1) % st->dash_count;
        }
        double left = (st->dash[index] - phase) * scale;
        bool on = index % 2 == 0;
        bool drawing = false;
        int segments = path->closed[sp] ? n : n - 1;
        for (int k = 0; k < segments; k++) {
            int i = start + k, j = start + (k + 1) % n;
            double x0 = path->xy[2 * i], y0 = path->xy[2 * i + 1];
            double x1 = path->xy[2 * j], y1 = path->xy[2 * j + 1];
            double length = vec_sqrt((x1 - x0) * (x1 - x0)
                                     + (y1 - y0) * (y1 - y0));
            double done = 0;
            while (done < length) {
                double step = left < length - done ? left : length - done;
                double ax = x0 + (x1 - x0) * done / length;
                double ay = y0 + (y1 - y0) * done / length;
                double bx = x0 + (x1 - x0) * (done + step) / length;
                double by = y0 + (y1 - y0) * (done + step) / length;
                if (on) {
                    if (!drawing) {
                        vec_move(&b, ax, ay);
                        drawing = true;
                    }
                    vec_lineto(&b, bx, by);
                }
                done += step;
                left -= step;
                if (left <= 1e-9) {
                    index = (index + 1) % st->dash_count;
                    left = st->dash[index] * scale;
                    on = !on;
                    drawing = false;
                    if (st->dash_count % 2 == 1 && index == 0) {
                        on = true; /* odd lists repeat twice */
                    }
                }
            }
        }
    }
}

/* Adds the stroke of path to the raster; returns the alpha factor
   (hairlines are drawn fainter). */
static float raster_stroke(struct canvas *c, const struct path *path) {
    const struct state *st = &c->st;
    double half = st->line_width * device_scale(c) / 2;
    float factor = 1;
    if (half < 0.35) {
        factor = (float)(half / 0.35);
        half = 0.35;
    }
    const struct path *use = path;
    struct path dashed = {0};
    if (st->dash_count > 0) {
        dash_path(c, path, &dashed);
        use = &dashed;
    }
    vec_stroke(&c->raster, use, st->cap, st->join, st->miter_limit, half);
    vec_path_free(&dashed);
    return factor;
}

/* ----------------------------------------------------------------- text */

static void parse_font(struct state *st, const char *text) {
    int size = -1;
    bool bold = false, italic = false, mono = false;
    const char *p = text;
    while (*p != '\0') {
        while (*p == ' ' || *p == ',') {
            p++;
        }
        const char *word = p;
        while (*p != '\0' && *p != ' ' && *p != ',') {
            p++;
        }
        size_t n = (size_t)(p - word);
        if (n == 0) {
            break;
        }
        if ((n == 4 && strncasecmp(word, "bold", 4) == 0)
            || (n == 6 && strncasecmp(word, "bolder", 6) == 0)
            || (n == 3 && word[0] >= '6' && word[0] <= '9'
                && word[1] == '0' && word[2] == '0')) {
            bold = true;
        } else if ((n == 6 && strncasecmp(word, "italic", 6) == 0)
                   || (n == 7 && strncasecmp(word, "oblique", 7) == 0)) {
            italic = true;
        } else if (size < 0 && word[0] >= '0' && word[0] <= '9') {
            double value = 0, unit = 1;
            const char *q = word;
            while (q < p && *q >= '0' && *q <= '9') {
                value = value * 10 + (*q++ - '0');
            }
            if (q < p && *q == '.') {
                q++;
                while (q < p && *q >= '0' && *q <= '9') {
                    value += (*q++ - '0') * (unit /= 10);
                }
            }
            if (q < p && *q != '/') {
                if (p - q >= 2 && strncasecmp(q, "pt", 2) == 0) {
                    value = value * 4 / 3;
                } else if (p - q >= 3 && strncasecmp(q, "rem", 3) == 0) {
                    value *= 16;
                } else if (p - q >= 2 && strncasecmp(q, "em", 2) == 0) {
                    value *= 16;
                } else if (*q == '%') {
                    value = value * 16 / 100;
                } else if (!(p - q >= 2 && strncasecmp(q, "px", 2) == 0)) {
                    continue;
                }
            }
            size = (int)(value + 0.5);
        } else if (size >= 0) {
            /* Family names come after the size. */
            for (size_t i = 0; i + 4 <= n; i++) {
                if (strncasecmp(word + i, "mono", 4) == 0
                    || strncasecmp(word + i, "cour", 4) == 0
                    || strncasecmp(word + i, "cons", 4) == 0) {
                    mono = true;
                }
            }
        }
    }
    if (size < 0) {
        return; /* not a font: ignored, as in browsers */
    }
    st->font_size = size < 1 ? 1 : size > 1000 ? 1000 : size;
    st->font_index = mono ? (bold ? 5 : 4) : (bold ? 1 : 0) + (italic ? 2 : 0);
}

static void draw_text(struct canvas *c, const char *text, size_t length,
                      double x, double y, double max_width,
                      const struct paint *paint) {
    const struct state *st = &c->st;
    struct opos_font *font = browser_font(st->font_index);
    if (font == NULL || length == 0) {
        return;
    }
    double scale = device_scale(c);
    int size = (int)(st->font_size * scale + 0.5);
    if (size < 1) {
        return;
    }
    if (size > 1000) {
        size = 1000;
    }
    int width = opos_font_text_width(font, size, text, length);
    if (max_width >= 0 && width > max_width * scale) {
        if (max_width <= 0) {
            return;
        }
        size = (int)(size * max_width * scale / width);
        if (size < 1) {
            return;
        }
        width = opos_font_text_width(font, size, text, length);
    }
    int ascent = opos_font_ascent(font, size);
    int descent = opos_font_descent(font, size);
    double dx, dy;
    vec_apply(&st->m, x, y, &dx, &dy);
    int align = st->align;
    if (align == 1 || align == 3) {
        dx -= width;
    } else if (align == 4) {
        dx -= width / 2.0;
    }
    switch (st->baseline) {
    case 1: case 2: dy += ascent; break;
    case 3: dy += (ascent - descent) / 2.0; break;
    case 4: case 5: dy -= descent; break;
    default: break;
    }
    int margin = 2 + size / 4;
    int tw = width + 2 * margin, th = ascent + descent + 2 * margin;
    if ((size_t)tw * (size_t)th > CANVAS_MAX_PIXELS) {
        return;
    }
    struct opos_canvas temp = opos_canvas_create(tw, th);
    if (temp.pixels == NULL) {
        return;
    }
    opos_font_draw(font, &temp, margin, margin + ascent, size, text, length,
                   0x00ffffff);
    /* The white-on-black drawing is the coverage: write it to the raster
       as area deltas. */
    int left = (int)vec_floor(dx + 0.5) - margin;
    int top = (int)vec_floor(dy + 0.5) - ascent - margin;
    struct raster *ra = &c->raster;
    int stride = c->width + 2;
    for (int row = 0; row < th; row++) {
        int py = top + row;
        if (py < 0 || py >= c->height) {
            continue;
        }
        float *cover = ra->cover + (size_t)py * stride;
        float previous = 0;
        int last = -1;
        for (int col = 0; col < tw; col++) {
            int px = left + col;
            if (px < 0 || px >= c->width) {
                continue;
            }
            float value = ((temp.pixels[(size_t)row * temp.stride + col] >> 8)
                           & 255) / 255.0f;
            cover[px] += value - previous;
            previous = value;
            last = px;
        }
        if (last >= 0) {
            cover[last + 1] -= previous;
            if (py < ra->top) ra->top = py;
            if (py + 1 > ra->bottom) ra->bottom = py + 1;
        }
    }
    opos_canvas_destroy(&temp);
    composite(c, paint, false, 1);
}

/* ------------------------------------------------------------------ PNG */

static uint32_t crc_table[256];

static uint32_t crc32_of(uint32_t crc, const uint8_t *data, size_t length) {
    if (crc_table[1] == 0) {
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t v = n;
            for (int k = 0; k < 8; k++) {
                v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
            }
            crc_table[n] = v;
        }
    }
    crc = ~crc;
    for (size_t i = 0; i < length; i++) {
        crc = crc_table[(crc ^ data[i]) & 255] ^ (crc >> 8);
    }
    return ~crc;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* A PNG of the pixels with stored (uncompressed) deflate blocks. */
static uint8_t *png_encode(const uint32_t *pixels, int width, int height,
                           size_t *out_length) {
    size_t raw = (size_t)height * ((size_t)width * 4 + 1);
    size_t blocks = raw / 65535 + 1;
    size_t zlib = 2 + raw + blocks * 5 + 4;
    size_t total = 8 + 25 + 12 + zlib + 12;
    uint8_t *png = malloc(total);
    uint8_t *rows = malloc(raw);
    if (png == NULL || rows == NULL) {
        free(png);
        free(rows);
        return NULL;
    }
    uint8_t *r = rows;
    for (int y = 0; y < height; y++) {
        *r++ = 0;
        for (int x = 0; x < width; x++) {
            uint32_t p = pixels[(size_t)y * width + x];
            *r++ = (uint8_t)(p >> 16);
            *r++ = (uint8_t)(p >> 8);
            *r++ = (uint8_t)p;
            *r++ = (uint8_t)(p >> 24);
        }
    }
    static const uint8_t signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    uint8_t *p = png;
    memcpy(p, signature, 8);
    p += 8;
    put32(p, 13);
    memcpy(p + 4, "IHDR", 4);
    put32(p + 8, (uint32_t)width);
    put32(p + 12, (uint32_t)height);
    p[16] = 8;  /* bit depth */
    p[17] = 6;  /* RGBA */
    p[18] = p[19] = p[20] = 0;
    put32(p + 21, crc32_of(0, p + 4, 17));
    p += 25;
    put32(p, (uint32_t)zlib);
    memcpy(p + 4, "IDAT", 4);
    uint8_t *data = p + 8, *z = data;
    *z++ = 0x78;
    *z++ = 0x01;
    uint32_t s1 = 1, s2 = 0;
    for (size_t at = 0; at < raw || at == 0;) {
        size_t n = raw - at < 65535 ? raw - at : 65535;
        *z++ = at + n >= raw ? 1 : 0;
        *z++ = (uint8_t)n;
        *z++ = (uint8_t)(n >> 8);
        *z++ = (uint8_t)~n;
        *z++ = (uint8_t)(~n >> 8);
        memcpy(z, rows + at, n);
        for (size_t i = 0; i < n; i++) {
            s1 = (s1 + rows[at + i]) % 65521;
            s2 = (s2 + s1) % 65521;
        }
        z += n;
        at += n;
        if (n == 0) {
            break;
        }
    }
    put32(z, s2 << 16 | s1);
    z += 4;
    size_t zlength = (size_t)(z - data);
    put32(p, (uint32_t)zlength);
    put32(z, crc32_of(0, p + 4, zlength + 4));
    p = z + 4;
    put32(p, 0);
    memcpy(p + 4, "IEND", 4);
    put32(p + 8, crc32_of(0, p + 4, 4));
    p += 12;
    free(rows);
    *out_length = (size_t)(p - png);
    return png;
}

static char *base64_url(const uint8_t *data, size_t length) {
    static const char digits[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const char *prefix = "data:image/png;base64,";
    size_t prefix_length = strlen(prefix);
    char *out = malloc(prefix_length + (length + 2) / 3 * 4 + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, prefix, prefix_length);
    char *o = out + prefix_length;
    for (size_t i = 0; i < length; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < length) v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < length) v |= data[i + 2];
        *o++ = digits[(v >> 18) & 63];
        *o++ = digits[(v >> 12) & 63];
        *o++ = i + 1 < length ? digits[(v >> 6) & 63] : '=';
        *o++ = i + 2 < length ? digits[v & 63] : '=';
    }
    *o = '\0';
    return out;
}

/* -------------------------------------------------------------- natives */

static double number(JSContext *ctx, int argc, JSValueConst *argv, int i) {
    double v = 0;
    if (i < argc) {
        JS_ToFloat64(ctx, &v, argv[i]);
    }
    return v;
}

static bool finite(double v) {
    return v == v && v < 1e300 && v > -1e300;
}

static bool numbers(JSContext *ctx, int argc, JSValueConst *argv, int first,
                    int count, double *out) {
    bool ok = true;
    for (int i = 0; i < count; i++) {
        out[i] = number(ctx, argc, argv, first + i);
        ok = ok && finite(out[i]);
    }
    return ok;
}

/* Sets a colour paint; *packed (if given) gets alpha << 24 | RGB. */
static bool set_colour(JSContext *ctx, JSValueConst value,
                       struct paint *paint, double *packed) {
    const char *text = JS_ToCString(ctx, value);
    if (text == NULL) {
        return false;
    }
    uint32_t colour;
    int alpha;
    bool ok = parse_colour_alpha(text, &colour, &alpha);
    if (!ok && strcasecmp(text, "transparent") == 0) {
        colour = 0;
        alpha = 0;
        ok = true;
    }
    JS_FreeCString(ctx, text);
    if (!ok) {
        return false;
    }
    pattern_release(paint->pattern);
    memset(paint, 0, sizeof(*paint));
    paint->kind = PAINT_COLOUR;
    colour_rgba(colour, alpha, paint->rgba);
    if (packed != NULL) {
        *packed = (double)alpha * 16777216.0 + (double)(colour & 0xffffff);
    }
    return true;
}

/* gradient: which, kind, x0, y0, r0, x1, y1, r1 (or angle), [offset,
   colour, ...] */
static bool set_gradient(JSContext *ctx, int argc, JSValueConst *argv,
                         struct paint *paint) {
    struct paint g;
    memset(&g, 0, sizeof(g));
    int kind = (int)number(ctx, argc, argv, 3);
    g.kind = kind == 1 ? PAINT_RADIAL : kind == 2 ? PAINT_CONIC : PAINT_LINEAR;
    double v[6];
    numbers(ctx, argc, argv, 4, 6, v);
    g.x0 = v[0]; g.y0 = v[1]; g.r0 = v[2];
    g.x1 = v[3]; g.y1 = v[4]; g.r1 = v[5];
    g.angle = v[2];
    if (argc > 10) {
        JSValue length_value = JS_GetPropertyStr(ctx, argv[10], "length");
        int32_t length = 0;
        JS_ToInt32(ctx, &length, length_value);
        JS_FreeValue(ctx, length_value);
        for (int32_t i = 0; i + 1 < length && g.stop_count < CANVAS_STOPS;
             i += 2) {
            JSValue o = JS_GetPropertyUint32(ctx, argv[10], (uint32_t)i);
            JSValue c = JS_GetPropertyUint32(ctx, argv[10], (uint32_t)i + 1);
            double offset = 0;
            JS_ToFloat64(ctx, &offset, o);
            struct paint stop;
            memset(&stop, 0, sizeof(stop));
            if (set_colour(ctx, c, &stop, NULL)) {
                g.offset[g.stop_count] = (float)offset;
                memcpy(g.stop[g.stop_count], stop.rgba, sizeof(float) * 4);
                g.stop_count++;
            }
            JS_FreeValue(ctx, o);
            JS_FreeValue(ctx, c);
        }
    }
    if (g.stop_count == 0) {
        g.kind = PAINT_COLOUR; /* transparent black */
    }
    /* Stops in order (stable: the later of equal offsets wins). */
    for (int i = 1; i < g.stop_count; i++) {
        for (int j = i; j > 0 && g.offset[j - 1] > g.offset[j]; j--) {
            float t = g.offset[j];
            g.offset[j] = g.offset[j - 1];
            g.offset[j - 1] = t;
            float s[4];
            memcpy(s, g.stop[j], sizeof(s));
            memcpy(g.stop[j], g.stop[j - 1], sizeof(s));
            memcpy(g.stop[j - 1], s, sizeof(s));
        }
    }
    pattern_release(paint->pattern);
    *paint = g;
    return true;
}

static bool set_pattern(JSContext *ctx, struct canvas *c, int argc,
                        JSValueConst *argv, struct paint *paint) {
    struct opos_html_node *source = argc > 3 ? script_unwrap(argv[3]) : NULL;
    if (source == NULL) {
        return false;
    }
    int width, height;
    bool foreign;
    const uint32_t *pixels = source_pixels(c->page, source, &width, &height,
                                           &foreign);
    if (pixels == NULL) {
        return false;
    }
    struct pattern *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return false;
    }
    p->pixels = malloc((size_t)width * height * 4);
    if (p->pixels == NULL) {
        free(p);
        return false;
    }
    memcpy(p->pixels, pixels, (size_t)width * height * 4);
    p->refs = 1;
    p->width = width;
    p->height = height;
    const char *repeat = argc > 4 ? JS_ToCString(ctx, argv[4]) : NULL;
    p->repeat_x = repeat == NULL || strcmp(repeat, "repeat") == 0
               || strcmp(repeat, "repeat-x") == 0 || repeat[0] == '\0';
    p->repeat_y = repeat == NULL || strcmp(repeat, "repeat") == 0
               || strcmp(repeat, "repeat-y") == 0 || repeat[0] == '\0';
    JS_FreeCString(ctx, repeat);
    if (foreign) {
        c->tainted = true;
    }
    pattern_release(paint->pattern);
    memset(paint, 0, sizeof(*paint));
    paint->kind = PAINT_PATTERN;
    paint->pattern = p;
    return true;
}

static JSValue int_pair(JSContext *ctx, int a, int b) {
    JSValue array = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, array, 0, JS_NewInt32(ctx, a));
    JS_SetPropertyUint32(ctx, array, 1, JS_NewInt32(ctx, b));
    return array;
}

static JSValue image_data(JSContext *ctx, struct canvas *c, int argc,
                          JSValueConst *argv) {
    double v[4];
    if (!numbers(ctx, argc, argv, 2, 4, v)) {
        return JS_NULL;
    }
    int x = (int)vec_floor(v[0]), y = (int)vec_floor(v[1]);
    int w = (int)v[2], h = (int)v[3];
    if (w <= 0 || h <= 0 || (size_t)w * (size_t)h > CANVAS_MAX_PIXELS) {
        return JS_ThrowRangeError(ctx, "bad image data size");
    }
    uint8_t *bytes = calloc((size_t)w * h, 4);
    if (bytes == NULL) {
        return JS_ThrowOutOfMemory(ctx);
    }
    for (int row = 0; row < h; row++) {
        int sy = y + row;
        if (sy < 0 || sy >= c->height || c->pixels == NULL) {
            continue;
        }
        for (int col = 0; col < w; col++) {
            int sx = x + col;
            if (sx < 0 || sx >= c->width) {
                continue;
            }
            uint32_t p = c->pixels[(size_t)sy * c->width + sx];
            uint8_t *o = bytes + ((size_t)row * w + col) * 4;
            o[0] = (uint8_t)(p >> 16);
            o[1] = (uint8_t)(p >> 8);
            o[2] = (uint8_t)p;
            o[3] = (uint8_t)(p >> 24);
        }
    }
    JSValue buffer = JS_NewArrayBufferCopy(ctx, bytes, (size_t)w * h * 4);
    free(bytes);
    return buffer;
}

/* putImageData: buffer, width, height, dx, dy, dirty x, y, w, h. */
static void put_image_data(JSContext *ctx, struct canvas *c, int argc,
                           JSValueConst *argv) {
    size_t size;
    uint8_t *bytes = argc > 2 ? JS_GetArrayBuffer(ctx, &size, argv[2]) : NULL;
    double v[8];
    if (bytes == NULL || !numbers(ctx, argc, argv, 3, 8, v)
        || c->pixels == NULL) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    int w = (int)v[0], h = (int)v[1];
    if (w <= 0 || h <= 0 || size < (size_t)w * h * 4) {
        return;
    }
    int dx = (int)vec_floor(v[2]), dy = (int)vec_floor(v[3]);
    int x0 = (int)v[4], y0 = (int)v[5], x1 = x0 + (int)v[6], y1 = y0 + (int)v[7];
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    for (int row = y0; row < y1; row++) {
        int ty = dy + row;
        if (ty < 0 || ty >= c->height) {
            continue;
        }
        for (int col = x0; col < x1; col++) {
            int tx = dx + col;
            if (tx < 0 || tx >= c->width) {
                continue;
            }
            const uint8_t *s = bytes + ((size_t)row * w + col) * 4;
            c->pixels[(size_t)ty * c->width + tx] =
                s[3] == 0 ? 0 : (uint32_t)s[3] << 24 | (uint32_t)s[0] << 16
                                | (uint32_t)s[1] << 8 | s[2];
        }
    }
    c->version++;
    painted = true;
}

/* drawImage: source, sx, sy, sw, sh, dx, dy, dw, dh (all given). */
static void draw_image(JSContext *ctx, struct canvas *c, int argc,
                       JSValueConst *argv) {
    struct opos_html_node *source = argc > 2 ? script_unwrap(argv[2]) : NULL;
    double v[8];
    if (source == NULL || !numbers(ctx, argc, argv, 3, 8, v)) {
        return;
    }
    int width, height;
    bool foreign;
    const uint32_t *pixels = source_pixels(c->page, source, &width, &height,
                                           &foreign);
    if (pixels == NULL || v[2] == 0 || v[3] == 0 || v[6] == 0 || v[7] == 0) {
        return;
    }
    uint32_t *copy = NULL;
    if (pixels == c->pixels) {
        copy = malloc((size_t)width * height * 4);
        if (copy == NULL) {
            return;
        }
        memcpy(copy, pixels, (size_t)width * height * 4);
        pixels = copy;
    }
    if (foreign) {
        c->tainted = true;
    }
    /* Only the part of the source rectangle inside the image is drawn. */
    double sx = v[0], sy = v[1], sw = v[2], sh = v[3];
    double dx = v[4], dy = v[5], dw = v[6], dh = v[7];
    double cx0 = sx < 0 ? 0 : sx, cy0 = sy < 0 ? 0 : sy;
    double cx1 = sx + sw > width ? width : sx + sw;
    double cy1 = sy + sh > height ? height : sy + sh;
    if (cx1 > cx0 && cy1 > cy0) {
        struct paint paint;
        memset(&paint, 0, sizeof(paint));
        paint.kind = PAINT_IMAGE;
        paint.pixels = pixels;
        paint.width = width;
        paint.height = height;
        paint.sx = sx; paint.sy = sy; paint.sw = sw; paint.sh = sh;
        paint.dx = dx; paint.dy = dy; paint.dw = dw; paint.dh = dh;
        double rx = dx + (cx0 - sx) * dw / sw, ry = dy + (cy0 - sy) * dh / sh;
        double rw = (cx1 - cx0) * dw / sw, rh = (cy1 - cy0) * dh / sh;
        scratch_rect(c, rx, ry, rw, rh);
        raster_path(&c->raster, &c->scratch);
        composite(c, &paint, false, 1);
    }
    free(copy);
}

static JSValue load_image_native(JSContext *ctx, struct page *page,
                                 struct opos_html_node *node, int argc,
                                 JSValueConst *argv) {
    const char *url = argc > 2 ? JS_ToCString(ctx, argv[2]) : NULL;
    if (url == NULL) {
        return JS_NULL;
    }
    bool foreign;
    struct image_ref *ref = load_image(page, url, &foreign);
    JS_FreeCString(ctx, url);
    script_more_time(ctx);
    for (struct loaded **p = &loaded_images; *p != NULL; p = &(*p)->next) {
        if ((*p)->node == node && (*p)->page == page) {
            struct loaded *old = *p;
            *p = old->next;
            image_ref_free(old->ref);
            free(old);
            break;
        }
    }
    if (ref == NULL) {
        return JS_NULL;
    }
    struct loaded *l = calloc(1, sizeof(*l));
    if (l == NULL) {
        image_ref_free(ref);
        return JS_NULL;
    }
    l->page = page;
    l->node = node;
    l->ref = ref;
    l->foreign = foreign;
    l->next = loaded_images;
    loaded_images = l;
    return int_pair(ctx, ref->width, ref->height);
}

/* N.cv(node, op, ...) */
static JSValue n_canvas(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv) {
    (void)this_val;
    struct opos_html_node *node = argc > 0 ? script_unwrap(argv[0]) : NULL;
    struct page *page = script_page(ctx);
    if (node == NULL || page == NULL || argc < 2) {
        return JS_ThrowTypeError(ctx, "not a node");
    }
    int op = (int)number(ctx, argc, argv, 1);
    if (op == OP_IMAGE_SIZE) {
        int width, height;
        bool foreign;
        if (source_pixels(page, node, &width, &height, &foreign) == NULL) {
            return JS_NULL;
        }
        return int_pair(ctx, width, height);
    }
    if (op == OP_LOAD_IMAGE) {
        return load_image_native(ctx, page, node, argc, argv);
    }
    if (node->tag != HTML_TAG_CANVAS) {
        return JS_ThrowTypeError(ctx, "not a canvas");
    }
    struct canvas *c = canvas_get(page, node);
    if (c == NULL) {
        return JS_ThrowOutOfMemory(ctx);
    }
    struct state *st = &c->st;
    double v[8];
    switch (op) {
    case OP_SIZE:
        return int_pair(ctx, c->width, c->height);
    case OP_RESET:
        canvas_clear_state(c);
        if (c->pixels != NULL) {
            memset(c->pixels, 0, (size_t)c->width * c->height * 4);
        }
        c->tainted = false;
        c->version++;
        painted = true;
        break;
    case OP_SAVE:
        if (c->depth < CANVAS_STACK) {
            c->stack[c->depth] = *st;
            state_hold(&c->stack[c->depth]);
            c->depth++;
        }
        break;
    case OP_RESTORE:
        if (c->depth > 0) {
            state_drop(st);
            *st = c->stack[--c->depth];
        }
        break;
    case OP_TRANSFORM:
    case OP_SET_TRANSFORM:
        if (numbers(ctx, argc, argv, 2, 6, v)) {
            struct matrix m = {v[0], v[1], v[2], v[3], v[4], v[5]};
            st->m = op == OP_TRANSFORM ? vec_multiply(st->m, m) : m;
        }
        break;
    case OP_GET_TRANSFORM: {
        JSValue array = JS_NewArray(ctx);
        double m[6] = {st->m.a, st->m.b, st->m.c, st->m.d, st->m.e, st->m.f};
        for (int i = 0; i < 6; i++) {
            JS_SetPropertyUint32(ctx, array, (uint32_t)i,
                                 JS_NewFloat64(ctx, m[i]));
        }
        return array;
    }
    case OP_BEGIN_PATH:
        vec_path_reset(&c->path);
        c->build.open = false;
        c->have_point = false;
        break;
    case OP_MOVE:
        if (numbers(ctx, argc, argv, 2, 2, v)) {
            path_move_to(c, v[0], v[1]);
        }
        break;
    case OP_LINE:
        if (numbers(ctx, argc, argv, 2, 2, v)) {
            path_line_to(c, v[0], v[1]);
        }
        break;
    case OP_QUAD:
        if (numbers(ctx, argc, argv, 2, 4, v)) {
            if (!c->have_point) {
                path_move_to(c, v[0], v[1]);
            }
            builder_ready(c, &c->path);
            vec_quad(&c->build, v[0], v[1], v[2], v[3]);
        }
        break;
    case OP_CUBIC:
        if (numbers(ctx, argc, argv, 2, 6, v)) {
            if (!c->have_point) {
                path_move_to(c, v[0], v[1]);
            }
            builder_ready(c, &c->path);
            vec_cubic(&c->build, v[0], v[1], v[2], v[3], v[4], v[5]);
        }
        break;
    case OP_ARC:
        if (numbers(ctx, argc, argv, 2, 5, v)) {
            if (v[2] < 0) {
                return JS_ThrowRangeError(ctx, "negative radius");
            }
            bool ccw = argc > 7 && JS_ToBool(ctx, argv[7]);
            builder_ready(c, &c->path);
            arc_points(c, v[0], v[1], v[2], v[2], 0, v[3],
                       arc_sweep(v[3], v[4], ccw));
        }
        break;
    case OP_ELLIPSE:
        if (numbers(ctx, argc, argv, 2, 7, v)) {
            if (v[2] < 0 || v[3] < 0) {
                return JS_ThrowRangeError(ctx, "negative radius");
            }
            bool ccw = argc > 9 && JS_ToBool(ctx, argv[9]);
            builder_ready(c, &c->path);
            arc_points(c, v[0], v[1], v[2], v[3], v[4], v[5],
                       arc_sweep(v[5], v[6], ccw));
        }
        break;
    case OP_ARC_TO:
        if (numbers(ctx, argc, argv, 2, 5, v)) {
            if (v[4] < 0) {
                return JS_ThrowRangeError(ctx, "negative radius");
            }
            builder_ready(c, &c->path);
            path_arc_to(c, v[0], v[1], v[2], v[3], v[4]);
        }
        break;
    case OP_RECT:
        if (numbers(ctx, argc, argv, 2, 4, v)) {
            path_move_to(c, v[0], v[1]);
            vec_lineto(&c->build, v[0] + v[2], v[1]);
            vec_lineto(&c->build, v[0] + v[2], v[1] + v[3]);
            vec_lineto(&c->build, v[0], v[1] + v[3]);
            vec_close(&c->build);
            path_move_to(c, v[0], v[1]);
        }
        break;
    case OP_CLOSE:
        if (c->have_point) {
            vec_close(&c->build);
        }
        break;
    case OP_FILL:
        if (c->pixels != NULL) {
            raster_path(&c->raster, &c->path);
            composite(c, &st->fill, argc > 2 && JS_ToBool(ctx, argv[2]), 1);
        }
        break;
    case OP_STROKE:
        if (c->pixels != NULL && !c->path.failed) {
            float factor = raster_stroke(c, &c->path);
            composite(c, &st->stroke, false, factor);
        }
        break;
    case OP_CLIP:
        if (c->pixels != NULL) {
            raster_path(&c->raster, &c->path);
            float *mask = take_cover(c, argc > 2 && JS_ToBool(ctx, argv[2]));
            struct clip *clip = calloc(1, sizeof(*clip));
            if (mask == NULL || clip == NULL) {
                free(mask);
                free(clip);
                break;
            }
            if (st->clip != NULL) {
                size_t n = (size_t)c->width * c->height;
                for (size_t i = 0; i < n; i++) {
                    mask[i] *= st->clip->cover[i];
                }
            }
            clip->refs = 1;
            clip->cover = mask;
            clip_release(st->clip);
            st->clip = clip;
        }
        break;
    case OP_FILL_RECT:
    case OP_STROKE_RECT:
    case OP_CLEAR_RECT:
        if (c->pixels != NULL && numbers(ctx, argc, argv, 2, 4, v)) {
            scratch_rect(c, v[0], v[1], v[2], v[3]);
            if (op == OP_FILL_RECT) {
                raster_path(&c->raster, &c->scratch);
                composite(c, &st->fill, false, 1);
            } else if (op == OP_STROKE_RECT) {
                float factor = raster_stroke(c, &c->scratch);
                composite(c, &st->stroke, false, factor);
            } else {
                /* clearRect: transparent black through the clip, without
                   globalAlpha or the compositing operator. */
                struct state saved = *st;
                st->alpha = 1;
                st->composite = CO_DESTINATION_OUT;
                struct paint black;
                memset(&black, 0, sizeof(black));
                black.kind = PAINT_COLOUR;
                black.rgba[3] = 1;
                raster_path(&c->raster, &c->scratch);
                composite(c, &black, false, 1);
                st->alpha = saved.alpha;
                st->composite = saved.composite;
            }
        }
        break;
    case OP_STYLE:
        if (argc > 3) {
            /* which: 0 fill, 1 stroke, 2 only parse (gradient stops) */
            int which = (int)number(ctx, argc, argv, 2);
            struct paint parsed;
            memset(&parsed, 0, sizeof(parsed));
            struct paint *paint = which == 0 ? &st->fill
                                : which == 1 ? &st->stroke : &parsed;
            double packed;
            if (!set_colour(ctx, argv[3], paint, &packed)) {
                return JS_NULL;
            }
            return JS_NewFloat64(ctx, packed);
        }
        break;
    case OP_GRADIENT: {
        struct paint *paint = number(ctx, argc, argv, 2) != 0
                            ? &st->stroke : &st->fill;
        set_gradient(ctx, argc, argv, paint);
        break;
    }
    case OP_PATTERN: {
        struct paint *paint = number(ctx, argc, argv, 2) != 0
                            ? &st->stroke : &st->fill;
        return JS_NewBool(ctx, set_pattern(ctx, c, argc, argv, paint));
    }
    case OP_LINE_WIDTH:
        v[0] = number(ctx, argc, argv, 2);
        if (finite(v[0]) && v[0] > 0) {
            st->line_width = v[0];
        }
        break;
    case OP_CAP:
        st->cap = (int)number(ctx, argc, argv, 2);
        break;
    case OP_JOIN:
        st->join = (int)number(ctx, argc, argv, 2);
        break;
    case OP_MITER:
        v[0] = number(ctx, argc, argv, 2);
        if (finite(v[0]) && v[0] > 0) {
            st->miter_limit = v[0];
        }
        break;
    case OP_ALPHA:
        v[0] = number(ctx, argc, argv, 2);
        if (finite(v[0]) && v[0] >= 0 && v[0] <= 1) {
            st->alpha = (float)v[0];
        }
        break;
    case OP_COMPOSITE:
        st->composite = (int)number(ctx, argc, argv, 2);
        break;
    case OP_FONT:
        if (argc > 2) {
            const char *text = JS_ToCString(ctx, argv[2]);
            if (text != NULL) {
                parse_font(st, text);
                JS_FreeCString(ctx, text);
            }
        }
        break;
    case OP_ALIGN:
        st->align = (int)number(ctx, argc, argv, 2);
        break;
    case OP_BASELINE:
        st->baseline = (int)number(ctx, argc, argv, 2);
        break;
    case OP_DASH: {
        st->dash_count = 0;
        if (argc <= 2) {
            break;
        }
        JSValue length_value = JS_GetPropertyStr(ctx, argv[2], "length");
        int32_t length = 0;
        JS_ToInt32(ctx, &length, length_value);
        JS_FreeValue(ctx, length_value);
        for (int32_t i = 0; i < length && i < CANVAS_DASHES; i++) {
            JSValue item = JS_GetPropertyUint32(ctx, argv[2], (uint32_t)i);
            double d = 0;
            JS_ToFloat64(ctx, &d, item);
            JS_FreeValue(ctx, item);
            st->dash[st->dash_count++] = finite(d) && d > 0 ? d : 0;
        }
        break;
    }
    case OP_DASH_OFFSET:
        v[0] = number(ctx, argc, argv, 2);
        if (finite(v[0])) {
            st->dash_offset = v[0];
        }
        break;
    case OP_SMOOTHING:
        st->smoothing = argc > 2 && JS_ToBool(ctx, argv[2]);
        break;
    case OP_FILL_TEXT:
    case OP_STROKE_TEXT:
    case OP_MEASURE: {
        size_t length;
        const char *text = argc > 2 ? JS_ToCStringLen(ctx, &length, argv[2])
                                    : NULL;
        if (text == NULL) {
            return JS_EXCEPTION;
        }
        JSValue result = JS_UNDEFINED;
        if (op == OP_MEASURE) {
            struct opos_font *font = browser_font(st->font_index);
            int size = st->font_size;
            JSValue array = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, array, 0, JS_NewInt32(ctx,
                font != NULL ? opos_font_text_width(font, size, text, length)
                             : 0));
            JS_SetPropertyUint32(ctx, array, 1, JS_NewInt32(ctx,
                font != NULL ? opos_font_ascent(font, size) : 0));
            JS_SetPropertyUint32(ctx, array, 2, JS_NewInt32(ctx,
                font != NULL ? opos_font_descent(font, size) : 0));
            result = array;
        } else if (c->pixels != NULL && numbers(ctx, argc, argv, 3, 2, v)) {
            double max_width = argc > 5 && !JS_IsUndefined(argv[5])
                             ? number(ctx, argc, argv, 5) : -1;
            draw_text(c, text, length, v[0], v[1],
                      finite(max_width) ? max_width : -1,
                      op == OP_FILL_TEXT ? &st->fill : &st->stroke);
        }
        JS_FreeCString(ctx, text);
        return result;
    }
    case OP_DRAW_IMAGE:
        if (c->pixels != NULL) {
            draw_image(ctx, c, argc, argv);
        }
        break;
    case OP_GET_IMAGE_DATA:
        if (c->tainted) {
            return JS_NULL;
        }
        return image_data(ctx, c, argc, argv);
    case OP_PUT_IMAGE_DATA:
        put_image_data(ctx, c, argc, argv);
        break;
    case OP_TO_DATA_URL: {
        if (c->tainted) {
            return JS_NULL;
        }
        if (c->pixels == NULL) {
            return JS_NewString(ctx, "data:,");
        }
        size_t length;
        uint8_t *png = png_encode(c->pixels, c->width, c->height, &length);
        char *url = png != NULL ? base64_url(png, length) : NULL;
        free(png);
        if (url == NULL) {
            return JS_ThrowOutOfMemory(ctx);
        }
        JSValue result = JS_NewString(ctx, url);
        free(url);
        return result;
    }
    case OP_IN_PATH:
    case OP_IN_STROKE: {
        if (c->pixels == NULL || !numbers(ctx, argc, argv, 2, 2, v)) {
            return JS_FALSE;
        }
        /* The point is in canvas units (not transformed). */
        int x = (int)vec_floor(v[0]), y = (int)vec_floor(v[1]);
        if (x < 0 || y < 0 || x >= c->width || y >= c->height) {
            return JS_FALSE;
        }
        if (op == OP_IN_PATH) {
            raster_path(&c->raster, &c->path);
        } else {
            raster_stroke(c, &c->path);
        }
        float *mask = take_cover(c, op == OP_IN_PATH && argc > 4
                                    && JS_ToBool(ctx, argv[4]));
        bool inside = mask != NULL && mask[(size_t)y * c->width + x] >= 0.5f;
        free(mask);
        return JS_NewBool(ctx, inside);
    }
    default:
        break;
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry canvas_functions[] = {
    JS_CFUNC_DEF("cv", 2, n_canvas),
};

void canvas_add_natives(JSContext *ctx, JSValue natives) {
    JS_SetPropertyFunctionList(ctx, natives, canvas_functions,
                               sizeof(canvas_functions)
                               / sizeof(canvas_functions[0]));
}
