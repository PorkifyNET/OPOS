/*
 * SVG images: <img src="x.svg">, CSS url(x.svg), data: URIs and <svg>
 * elements in the page. The document comes from opos_html_parse, which
 * keeps SVG elements and their (lower-cased) attributes as it finds them.
 *
 * Supported: <svg> (nested, viewBox, preserveAspectRatio), <g>, <a>,
 * <switch>, <path>, <rect>, <circle>, <ellipse>, <line>, <polyline>,
 * <polygon>, <use> with <symbol>, transforms, fill and stroke (colour,
 * currentColor, linear and radial gradients), opacity, fill-rule, line
 * caps and joins, presentation attributes, style="" and simple <style>
 * rules (.class, #id, element). Not supported: text, clipping, masks,
 * patterns, filters, markers and embedded images; those parts are left
 * out.
 *
 * Shapes are flattened to polygons and drawn with an exact-area
 * anti-aliasing rasteriser (signed area accumulation per pixel row).
 */
#include <stdlib.h>
#include <string.h>

#include "browser.h"

#define SVG_MAX_DEPTH 24
#define SVG_MAX_RULES 256
#define SVG_MAX_STOPS 16
#define SVG_PI 3.14159265358979323846

/* ------------------------------------------------------------------ math */

static double svg_sqrt(double x) {
    if (x <= 0) {
        return 0;
    }
    double result;
    __asm__("sqrtsd %1, %0" : "=x"(result) : "x"(x));
    return result;
}

static double svg_fabs(double x) { return x < 0 ? -x : x; }

static double svg_floor(double x) {
    double i = (double)(long long)x;
    return i > x ? i - 1 : i;
}

static double svg_ceil(double x) {
    double i = (double)(long long)x;
    return i < x ? i + 1 : i;
}

/* sin for |x| <= pi/4 and cos for |x| <= pi/4 (Taylor series). */
static double sin_small(double x) {
    double x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72
           * (1 - x2 / 110)))));
}

static double cos_small(double x) {
    double x2 = x * x;
    return 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56
           * (1 - x2 / 90))));
}

static double svg_sin(double x) {
    double quarter = svg_floor(x / (SVG_PI / 2) + 0.5);
    double r = x - quarter * (SVG_PI / 2);
    switch (((long long)quarter % 4 + 4) % 4) {
    case 0: return sin_small(r);
    case 1: return cos_small(r);
    case 2: return -sin_small(r);
    default: return -cos_small(r);
    }
}

static double svg_cos(double x) { return svg_sin(x + SVG_PI / 2); }

static double svg_atan(double x) {
    /* Two half-angle reductions bring |x| below tan(pi/16), where the
       series converges quickly. */
    bool invert = svg_fabs(x) > 1;
    double t = invert ? 1 / x : x;
    t = t / (1 + svg_sqrt(1 + t * t));
    t = t / (1 + svg_sqrt(1 + t * t));
    double t2 = t * t;
    double result = 4 * t * (1 - t2 / 3 + t2 * t2 / 5 - t2 * t2 * t2 / 7
                             + t2 * t2 * t2 * t2 / 9);
    if (invert) {
        result = (x > 0 ? SVG_PI / 2 : -SVG_PI / 2) - result;
    }
    return result;
}

static double svg_atan2(double y, double x) {
    if (x > 0) {
        return svg_atan(y / x);
    }
    if (x < 0) {
        return svg_atan(y / x) + (y >= 0 ? SVG_PI : -SVG_PI);
    }
    return y > 0 ? SVG_PI / 2 : y < 0 ? -SVG_PI / 2 : 0;
}

/* --------------------------------------------------------------- numbers */

static const char *skip_separators(const char *p) {
    while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    return p;
}

/* Reads a number ("-1.5e3", ".5"); false when there is none. */
static bool read_number(const char **text, double *out) {
    const char *p = skip_separators(*text);
    double sign = 1;
    if (*p == '-' || *p == '+') {
        sign = *p == '-' ? -1 : 1;
        p++;
    }
    double value = 0;
    bool digits = false;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p++ - '0');
        digits = true;
    }
    if (*p == '.') {
        p++;
        double scale = 0.1;
        while (*p >= '0' && *p <= '9') {
            value += (*p++ - '0') * scale;
            scale *= 0.1;
            digits = true;
        }
    }
    if (!digits) {
        return false;
    }
    if ((*p == 'e' || *p == 'E')
        && (p[1] == '-' || p[1] == '+' || (p[1] >= '0' && p[1] <= '9'))) {
        p++;
        int esign = 1;
        if (*p == '-' || *p == '+') {
            esign = *p == '-' ? -1 : 1;
            p++;
        }
        int exponent = 0;
        while (*p >= '0' && *p <= '9') {
            if (exponent < 400) {
                exponent = exponent * 10 + (*p - '0');
            }
            p++;
        }
        while (exponent-- > 0) {
            value = esign > 0 ? value * 10 : value / 10;
        }
    }
    *out = sign * value;
    *text = p;
    return true;
}

/* A length ("12", "12px", "1em", "50%"). percent_of is what 100% means;
   false when there is no number. */
static bool read_length(const char *text, double percent_of, double *out) {
    if (text == NULL) {
        return false;
    }
    double value;
    if (!read_number(&text, &value)) {
        return false;
    }
    if (*text == '%') {
        value = value * percent_of / 100;
    } else if (strncmp(text, "pt", 2) == 0) {
        value = value * 4 / 3;
    } else if (strncmp(text, "pc", 2) == 0) {
        value *= 16;
    } else if (strncmp(text, "em", 2) == 0 || strncmp(text, "rem", 3) == 0) {
        value *= 16;
    } else if (strncmp(text, "ex", 2) == 0) {
        value *= 8;
    } else if (strncmp(text, "mm", 2) == 0) {
        value = value * 96 / 25.4;
    } else if (strncmp(text, "cm", 2) == 0) {
        value = value * 96 / 2.54;
    } else if (strncmp(text, "in", 2) == 0) {
        value *= 96;
    }
    *out = value;
    return true;
}

static bool word_equal(const char *text, size_t length, const char *word) {
    size_t n = strlen(word);
    if (n != length) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char a = text[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a + 32);
        }
        if (a != word[i]) {
            return false;
        }
    }
    return true;
}

/* -------------------------------------------------------------- matrices */

/* x' = a x + c y + e, y' = b x + d y + f */
struct matrix {
    double a, b, c, d, e, f;
};

static const struct matrix IDENTITY = {1, 0, 0, 1, 0, 0};

/* The transform that applies n first, then m. */
static struct matrix multiply(struct matrix m, struct matrix n) {
    struct matrix r;
    r.a = m.a * n.a + m.c * n.b;
    r.b = m.b * n.a + m.d * n.b;
    r.c = m.a * n.c + m.c * n.d;
    r.d = m.b * n.c + m.d * n.d;
    r.e = m.a * n.e + m.c * n.f + m.e;
    r.f = m.b * n.e + m.d * n.f + m.f;
    return r;
}

static bool invert(struct matrix m, struct matrix *out) {
    double det = m.a * m.d - m.b * m.c;
    if (svg_fabs(det) < 1e-12) {
        return false;
    }
    out->a = m.d / det;
    out->b = -m.b / det;
    out->c = -m.c / det;
    out->d = m.a / det;
    out->e = (m.c * m.f - m.d * m.e) / det;
    out->f = (m.b * m.e - m.a * m.f) / det;
    return true;
}

static void apply(const struct matrix *m, double x, double y, double *ox,
                  double *oy) {
    *ox = m->a * x + m->c * y + m->e;
    *oy = m->b * x + m->d * y + m->f;
}

/* Parses a transform list ("translate(10 20) rotate(45)"). */
static struct matrix parse_transform(const char *text) {
    struct matrix result = IDENTITY;
    if (text == NULL) {
        return result;
    }
    const char *p = text;
    while (*p != '\0') {
        p = skip_separators(p);
        const char *name = p;
        while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) {
            p++;
        }
        size_t name_length = (size_t)(p - name);
        while (*p == ' ') {
            p++;
        }
        if (name_length == 0 || *p != '(') {
            break;
        }
        p++;
        double v[6] = {0};
        int n = 0;
        while (n < 6 && read_number(&p, &v[n])) {
            n++;
        }
        while (*p != '\0' && *p != ')') {
            p++;
        }
        if (*p == ')') {
            p++;
        }
        struct matrix t = IDENTITY;
        if (word_equal(name, name_length, "matrix") && n == 6) {
            t.a = v[0]; t.b = v[1]; t.c = v[2];
            t.d = v[3]; t.e = v[4]; t.f = v[5];
        } else if (word_equal(name, name_length, "translate") && n >= 1) {
            t.e = v[0];
            t.f = n >= 2 ? v[1] : 0;
        } else if (word_equal(name, name_length, "scale") && n >= 1) {
            t.a = v[0];
            t.d = n >= 2 ? v[1] : v[0];
        } else if (word_equal(name, name_length, "rotate") && n >= 1) {
            double angle = v[0] * SVG_PI / 180;
            double s = svg_sin(angle), co = svg_cos(angle);
            t.a = co; t.b = s; t.c = -s; t.d = co;
            if (n >= 3) {
                /* rotate(a, cx, cy) = translate(cx cy) rotate(a)
                   translate(-cx -cy) */
                t.e = v[1] - co * v[1] + s * v[2];
                t.f = v[2] - s * v[1] - co * v[2];
            }
        } else if (word_equal(name, name_length, "skewx") && n >= 1) {
            double angle = v[0] * SVG_PI / 180;
            t.c = svg_sin(angle) / svg_cos(angle);
        } else if (word_equal(name, name_length, "skewy") && n >= 1) {
            double angle = v[0] * SVG_PI / 180;
            t.b = svg_sin(angle) / svg_cos(angle);
        }
        result = multiply(result, t);
    }
    return result;
}

/* ------------------------------------------------------------ rasteriser */

struct raster {
    int width, height;
    float *cover;         /* (width + 2) * height signed area deltas */
    float *pixels;        /* premultiplied RGBA, width * height * 4 */
    int top, bottom;      /* rows that hold area deltas */
};

static void raster_line(struct raster *r, double x0, double y0, double x1,
                        double y1) {
    if (y0 == y1 || y0 != y0 || y1 != y1) {
        return;
    }
    float direction = 1;
    if (y0 > y1) {
        double t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
        direction = -1;
    }
    if (y1 <= 0 || y0 >= r->height) {
        return;
    }
    double dxdy = (x1 - x0) / (y1 - y0);
    double x = x0;
    if (y0 < 0) {
        x -= y0 * dxdy;
        y0 = 0;
    }
    int first = (int)y0;
    int last = (int)svg_ceil(y1);
    if (last > r->height) {
        last = r->height;
    }
    if (first < r->top) {
        r->top = first;
    }
    if (last > r->bottom) {
        r->bottom = last;
    }
    int stride = r->width + 2;
    double right = r->width;
    for (int y = first; y < last; y++) {
        float *row = r->cover + (size_t)y * stride;
        double dy = (y + 1 < y1 ? y + 1 : y1) - (y > y0 ? y : y0);
        double next = x + dxdy * dy;
        float d = (float)dy * direction;
        double xa = x < next ? x : next, xb = x < next ? next : x;
        xa = xa < 0 ? 0 : xa > right ? right : xa;
        xb = xb < 0 ? 0 : xb > right ? right : xb;
        double xa_floor = svg_floor(xa);
        int xai = (int)xa_floor;
        double xb_ceil = svg_ceil(xb);
        int xbi = (int)xb_ceil;
        if (xbi <= xai + 1) {
            float middle = (float)(0.5 * (xa + xb) - xa_floor);
            row[xai] += d - d * middle;
            row[xai + 1] += d * middle;
        } else {
            double s = 1 / (xb - xa);
            double xaf = xa - xa_floor;
            float a0 = (float)(0.5 * s * (1 - xaf) * (1 - xaf));
            double xbf = xb - xb_ceil + 1;
            float am = (float)(0.5 * s * xbf * xbf);
            row[xai] += d * a0;
            if (xbi == xai + 2) {
                row[xai + 1] += d * (1 - a0 - am);
            } else {
                float a1 = (float)(s * (1.5 - xaf));
                row[xai + 1] += d * (a1 - a0);
                for (int xi = xai + 2; xi < xbi - 1; xi++) {
                    row[xi] += d * (float)s;
                }
                float a2 = a1 + (float)((xbi - xai - 3) * s);
                row[xbi - 1] += d * (1 - a2 - am);
            }
            row[xbi] += d * am;
        }
        x = next;
    }
}

/* ----------------------------------------------------------------- paths */

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

static void path_reset(struct path *path) {
    path->count = 0;
    path->subpaths = 0;
    path->failed = false;
    path->have_box = false;
}

static void path_free(struct path *path) {
    free(path->xy);
    free(path->starts);
    free(path->closed);
    memset(path, 0, sizeof(*path));
}

static void path_box(struct path *path, double x, double y) {
    if (!path->have_box) {
        path->min_x = path->max_x = x;
        path->min_y = path->max_y = y;
        path->have_box = true;
        return;
    }
    if (x < path->min_x) path->min_x = x;
    if (x > path->max_x) path->max_x = x;
    if (y < path->min_y) path->min_y = y;
    if (y > path->max_y) path->max_y = y;
}

static void path_point(struct path *path, double x, double y) {
    if (path->failed) {
        return;
    }
    if (path->subpaths == 0) {
        return;
    }
    if (path->count == path->capacity) {
        int capacity = path->capacity == 0 ? 64 : path->capacity * 2;
        double *grown = realloc(path->xy, (size_t)capacity * 2 * sizeof(double));
        if (grown == NULL || capacity > 4 * 1024 * 1024) {
            path->failed = true;
            return;
        }
        path->xy = grown;
        path->capacity = capacity;
    }
    path->xy[2 * path->count] = x;
    path->xy[2 * path->count + 1] = y;
    path->count++;
}

static void path_move(struct path *path, double x, double y) {
    if (path->failed) {
        return;
    }
    /* A lone moveto (no lines) is replaced. */
    if (path->subpaths > 0
        && path->starts[path->subpaths - 1] == path->count - 1) {
        path->count--;
        path->subpaths--;
    }
    if (path->subpaths == path->sub_capacity) {
        int capacity = path->sub_capacity == 0 ? 8 : path->sub_capacity * 2;
        int *starts = realloc(path->starts, (size_t)capacity * sizeof(int));
        if (starts == NULL) {
            path->failed = true;
            return;
        }
        path->starts = starts;
        bool *closed = realloc(path->closed, (size_t)capacity * sizeof(bool));
        if (closed == NULL) {
            path->failed = true;
            return;
        }
        path->closed = closed;
        path->sub_capacity = capacity;
    }
    path->starts[path->subpaths] = path->count;
    path->closed[path->subpaths] = false;
    path->subpaths++;
    path_point(path, x, y);
}

static void path_close(struct path *path) {
    if (path->subpaths > 0) {
        path->closed[path->subpaths - 1] = true;
    }
}

static int subpath_end(const struct path *path, int index) {
    return index + 1 < path->subpaths ? path->starts[index + 1] : path->count;
}

/* Builds a path in user units, flattened through a matrix to device
   pixels. */
struct builder {
    struct path *path;
    struct matrix m;
    double x, y;           /* current point (user units) */
    double start_x, start_y;
    bool open;             /* a subpath has been started */
};

static void build_move(struct builder *b, double x, double y) {
    double dx, dy;
    apply(&b->m, x, y, &dx, &dy);
    path_move(b->path, dx, dy);
    path_box(b->path, x, y);
    b->x = b->start_x = x;
    b->y = b->start_y = y;
    b->open = true;
}

static void build_line(struct builder *b, double x, double y) {
    if (!b->open) {
        build_move(b, b->x, b->y);
    }
    double dx, dy;
    apply(&b->m, x, y, &dx, &dy);
    path_point(b->path, dx, dy);
    path_box(b->path, x, y);
    b->x = x;
    b->y = y;
}

static double device_distance(const struct builder *b, double x0, double y0,
                              double x1, double y1) {
    double ax, ay, bx, by;
    apply(&b->m, x0, y0, &ax, &ay);
    apply(&b->m, x1, y1, &bx, &by);
    return svg_sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
}

static int segments_for(double length) {
    int n = (int)(svg_sqrt(length) * 1.6) + 1;
    return n < 1 ? 1 : n > 200 ? 200 : n;
}

static void build_cubic(struct builder *b, double x1, double y1, double x2,
                        double y2, double x3, double y3) {
    if (!b->open) {
        build_move(b, b->x, b->y);
    }
    double x0 = b->x, y0 = b->y;
    double length = device_distance(b, x0, y0, x1, y1)
                  + device_distance(b, x1, y1, x2, y2)
                  + device_distance(b, x2, y2, x3, y3);
    int n = segments_for(length);
    path_box(b->path, x1, y1);
    path_box(b->path, x2, y2);
    for (int i = 1; i <= n; i++) {
        double t = (double)i / n, u = 1 - t;
        double px = u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2
                  + t * t * t * x3;
        double py = u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2
                  + t * t * t * y3;
        double dx, dy;
        apply(&b->m, px, py, &dx, &dy);
        path_point(b->path, dx, dy);
    }
    path_box(b->path, x3, y3);
    b->x = x3;
    b->y = y3;
}

static void build_quad(struct builder *b, double x1, double y1, double x2,
                       double y2) {
    double x0 = b->x, y0 = b->y;
    build_cubic(b, x0 + 2.0 / 3 * (x1 - x0), y0 + 2.0 / 3 * (y1 - y0),
                x2 + 2.0 / 3 * (x1 - x2), y2 + 2.0 / 3 * (y1 - y2), x2, y2);
}

/* The SVG elliptical arc from the current point (endpoint form, see the
   SVG specification's implementation notes F.6.5). */
static void build_arc(struct builder *b, double rx, double ry, double angle,
                      bool large, bool sweep, double x, double y) {
    double x0 = b->x, y0 = b->y;
    rx = svg_fabs(rx);
    ry = svg_fabs(ry);
    if (rx < 1e-9 || ry < 1e-9 || (x0 == x && y0 == y)) {
        build_line(b, x, y);
        return;
    }
    double phi = angle * SVG_PI / 180;
    double cp = svg_cos(phi), sp = svg_sin(phi);
    double hx = (x0 - x) / 2, hy = (y0 - y) / 2;
    double x1p = cp * hx + sp * hy;
    double y1p = -sp * hx + cp * hy;
    double lambda = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
    if (lambda > 1) {
        double s = svg_sqrt(lambda);
        rx *= s;
        ry *= s;
    }
    double num = rx * rx * ry * ry - rx * rx * y1p * y1p
               - ry * ry * x1p * x1p;
    double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    double coef = den > 0 && num > 0 ? svg_sqrt(num / den) : 0;
    if (large == sweep) {
        coef = -coef;
    }
    double cxp = coef * rx * y1p / ry;
    double cyp = -coef * ry * x1p / rx;
    double cx = cp * cxp - sp * cyp + (x0 + x) / 2;
    double cy = sp * cxp + cp * cyp + (y0 + y) / 2;
    double theta = svg_atan2((y1p - cyp) / ry, (x1p - cxp) / rx);
    double end = svg_atan2((-y1p - cyp) / ry, (-x1p - cxp) / rx);
    double delta = end - theta;
    if (sweep && delta < 0) {
        delta += 2 * SVG_PI;
    } else if (!sweep && delta > 0) {
        delta -= 2 * SVG_PI;
    }
    if (!b->open) {
        build_move(b, x0, y0);
    }
    double scale = svg_sqrt(svg_fabs(b->m.a * b->m.d - b->m.b * b->m.c));
    double length = svg_fabs(delta) * (rx > ry ? rx : ry) * scale;
    int n = segments_for(length) + 1;
    for (int i = 1; i <= n; i++) {
        double t = theta + delta * i / n;
        double ct = svg_cos(t), st = svg_sin(t);
        double px = cx + rx * ct * cp - ry * st * sp;
        double py = cy + rx * ct * sp + ry * st * cp;
        if (i == n) {
            px = x;
            py = y;
        }
        double dx, dy;
        apply(&b->m, px, py, &dx, &dy);
        path_point(b->path, dx, dy);
        path_box(b->path, px, py);
    }
    b->x = x;
    b->y = y;
}

static void build_close(struct builder *b) {
    if (b->open) {
        path_close(b->path);
    }
    b->x = b->start_x;
    b->y = b->start_y;
    b->open = false;
}

/* Reads one arc flag ("0" or "1"), which needs no separator after it. */
static bool read_flag(const char **text, bool *out) {
    const char *p = skip_separators(*text);
    if (*p != '0' && *p != '1') {
        return false;
    }
    *out = *p == '1';
    *text = p + 1;
    return true;
}

static void build_path_data(struct builder *b, const char *d) {
    if (d == NULL) {
        return;
    }
    const char *p = d;
    char command = 0;
    double last_cx = 0, last_cy = 0; /* reflected control point */
    char last = 0;
    while (true) {
        p = skip_separators(p);
        if (*p == '\0') {
            break;
        }
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) {
            command = *p++;
        } else if (command == 0) {
            break;
        }
        bool relative = command >= 'a';
        double ox = relative ? b->x : 0, oy = relative ? b->y : 0;
        double v[7];
        char upper = (char)(relative ? command - 32 : command);
        bool ok = true;
        switch (upper) {
        case 'Z':
            build_close(b);
            command = 0;
            break;
        case 'M':
        case 'L':
        case 'T':
            ok = read_number(&p, &v[0]) && read_number(&p, &v[1]);
            if (!ok) {
                break;
            }
            if (upper == 'M') {
                build_move(b, ox + v[0], oy + v[1]);
                command = relative ? 'l' : 'L';
            } else if (upper == 'L') {
                build_line(b, ox + v[0], oy + v[1]);
            } else {
                double cx = b->x, cy = b->y;
                if (last == 'Q' || last == 'T') {
                    cx = 2 * b->x - last_cx;
                    cy = 2 * b->y - last_cy;
                }
                build_quad(b, cx, cy, ox + v[0], oy + v[1]);
                last_cx = cx;
                last_cy = cy;
            }
            break;
        case 'H':
            ok = read_number(&p, &v[0]);
            if (ok) {
                build_line(b, ox + v[0], b->y);
            }
            break;
        case 'V':
            ok = read_number(&p, &v[0]);
            if (ok) {
                build_line(b, b->x, oy + v[0]);
            }
            break;
        case 'C':
            for (int i = 0; i < 6 && ok; i++) {
                ok = read_number(&p, &v[i]);
            }
            if (ok) {
                build_cubic(b, ox + v[0], oy + v[1], ox + v[2], oy + v[3],
                            ox + v[4], oy + v[5]);
                last_cx = ox + v[2];
                last_cy = oy + v[3];
            }
            break;
        case 'S':
            for (int i = 0; i < 4 && ok; i++) {
                ok = read_number(&p, &v[i]);
            }
            if (ok) {
                double cx = b->x, cy = b->y;
                if (last == 'C' || last == 'S') {
                    cx = 2 * b->x - last_cx;
                    cy = 2 * b->y - last_cy;
                }
                build_cubic(b, cx, cy, ox + v[0], oy + v[1], ox + v[2],
                            oy + v[3]);
                last_cx = ox + v[0];
                last_cy = oy + v[1];
            }
            break;
        case 'Q':
            for (int i = 0; i < 4 && ok; i++) {
                ok = read_number(&p, &v[i]);
            }
            if (ok) {
                build_quad(b, ox + v[0], oy + v[1], ox + v[2], oy + v[3]);
                last_cx = ox + v[0];
                last_cy = oy + v[1];
            }
            break;
        case 'A': {
            bool large = false, sweep = false;
            ok = read_number(&p, &v[0]) && read_number(&p, &v[1])
              && read_number(&p, &v[2]) && read_flag(&p, &large)
              && read_flag(&p, &sweep) && read_number(&p, &v[3])
              && read_number(&p, &v[4]);
            if (ok) {
                build_arc(b, v[0], v[1], v[2], large, sweep, ox + v[3],
                          oy + v[4]);
            }
            break;
        }
        default:
            ok = false;
            break;
        }
        if (!ok) {
            break; /* an error ends the path; what came before is drawn */
        }
        last = upper;
    }
}

/* ----------------------------------------------------------------- paint */

enum paint_kind { PAINT_NONE, PAINT_COLOUR, PAINT_GRADIENT };

struct paint {
    enum paint_kind kind;
    uint32_t colour;
    float alpha;                        /* 0..1, from the colour */
    bool current;                       /* currentColor */
    const struct opos_html_node *gradient;
    uint32_t fallback;                  /* url(#x) colour */
    bool has_fallback;
};

struct state {
    struct paint fill, stroke;
    float fill_opacity, stroke_opacity, opacity;
    double stroke_width;
    bool even_odd;
    int cap;                            /* 0 butt, 1 round, 2 square */
    int join;                           /* 0 miter, 1 round, 2 bevel */
    double miter_limit;
    uint32_t colour;                    /* currentColor */
    bool hidden;                        /* display: none */
    bool invisible;                     /* visibility: hidden */
};

struct rule {
    char kind;                          /* '.', '#' or 'e' (element) */
    const char *name;
    size_t name_length;
    const char *declarations;
    size_t declarations_length;
};

struct render {
    struct raster raster;
    struct path path;
    const struct opos_html_node *root;  /* the outermost <svg> */
    const struct opos_html_node *document; /* top of the tree */
    struct rule rules[SVG_MAX_RULES];
    int rule_count;
    char *style_text;                   /* <style> contents (copies) */
    double view_width, view_height;     /* for percentages */
    int depth;
};

static bool parse_paint(const char *text, struct paint *paint,
                        const struct render *r) {
    text = skip_separators(text);
    size_t length = strlen(text);
    while (length > 0 && text[length - 1] == ' ') {
        length--;
    }
    if (word_equal(text, length, "none")
        || word_equal(text, length, "transparent")) {
        paint->kind = PAINT_NONE;
        return true;
    }
    if (word_equal(text, length, "currentcolor")) {
        paint->kind = PAINT_COLOUR;
        paint->current = true;
        paint->alpha = 1;
        return true;
    }
    if (strncmp(text, "url(", 4) == 0) {
        const char *p = text + 4;
        while (*p == ' ' || *p == '\'' || *p == '"') {
            p++;
        }
        if (*p == '#') {
            p++;
        }
        const char *end = p;
        while (*end != '\0' && *end != ')' && *end != '\'' && *end != '"') {
            end++;
        }
        char id[96];
        size_t n = (size_t)(end - p);
        if (n >= sizeof(id)) {
            n = sizeof(id) - 1;
        }
        memcpy(id, p, n);
        id[n] = '\0';
        const struct opos_html_node *target = svg_find_id(r->root,
                                                          r->document, id);
        while (*end != '\0' && *end != ')') {
            end++;
        }
        if (*end == ')') {
            end++;
        }
        struct paint fallback = {0};
        bool has_fallback = *skip_separators(end) != '\0'
                         && parse_paint(end, &fallback, r)
                         && fallback.kind == PAINT_COLOUR;
        if (target != NULL && target->name != NULL
            && (strcmp(target->name, "lineargradient") == 0
                || strcmp(target->name, "radialgradient") == 0)) {
            paint->kind = PAINT_GRADIENT;
            paint->gradient = target;
            paint->has_fallback = has_fallback;
            paint->fallback = fallback.colour;
            return true;
        }
        if (has_fallback) {
            *paint = fallback;
            return true;
        }
        paint->kind = PAINT_NONE; /* patterns and missing references */
        return true;
    }
    uint32_t colour;
    int alpha;
    if (parse_colour_alpha(text, &colour, &alpha)) {
        paint->kind = alpha > 0 ? PAINT_COLOUR : PAINT_NONE;
        paint->colour = colour;
        paint->alpha = alpha / 255.0f;
        paint->current = false;
        return true;
    }
    return false;
}

static float parse_opacity(const char *text, float fallback) {
    double value;
    const char *p = text;
    if (!read_number(&p, &value)) {
        return fallback;
    }
    if (*p == '%') {
        value /= 100;
    }
    return value < 0 ? 0 : value > 1 ? 1 : (float)value;
}

/* Applies one property (presentation attribute or CSS declaration). */
static void apply_property(struct state *s, const char *name,
                           size_t name_length, const char *value,
                           const struct render *r) {
    if (word_equal(value, strlen(value), "inherit")) {
        return;
    }
    if (word_equal(name, name_length, "fill")) {
        parse_paint(value, &s->fill, r);
    } else if (word_equal(name, name_length, "stroke")) {
        parse_paint(value, &s->stroke, r);
    } else if (word_equal(name, name_length, "fill-opacity")) {
        s->fill_opacity = parse_opacity(value, s->fill_opacity);
    } else if (word_equal(name, name_length, "stroke-opacity")) {
        s->stroke_opacity = parse_opacity(value, s->stroke_opacity);
    } else if (word_equal(name, name_length, "opacity")) {
        s->opacity *= parse_opacity(value, 1);
    } else if (word_equal(name, name_length, "stroke-width")) {
        double width;
        if (read_length(value, svg_sqrt((r->view_width * r->view_width
                                         + r->view_height * r->view_height)
                                        / 2),
                        &width) && width >= 0) {
            s->stroke_width = width;
        }
    } else if (word_equal(name, name_length, "fill-rule")) {
        s->even_odd = strncmp(skip_separators(value), "evenodd", 7) == 0;
    } else if (word_equal(name, name_length, "stroke-linecap")) {
        value = skip_separators(value);
        s->cap = strncmp(value, "round", 5) == 0 ? 1
               : strncmp(value, "square", 6) == 0 ? 2 : 0;
    } else if (word_equal(name, name_length, "stroke-linejoin")) {
        value = skip_separators(value);
        s->join = strncmp(value, "round", 5) == 0 ? 1
                : strncmp(value, "bevel", 5) == 0 ? 2 : 0;
    } else if (word_equal(name, name_length, "stroke-miterlimit")) {
        double limit;
        const char *p = value;
        if (read_number(&p, &limit) && limit >= 1) {
            s->miter_limit = limit;
        }
    } else if (word_equal(name, name_length, "color")) {
        uint32_t colour;
        if (parse_colour(value, &colour)) {
            s->colour = colour;
        }
    } else if (word_equal(name, name_length, "display")) {
        s->hidden = strncmp(skip_separators(value), "none", 4) == 0;
    } else if (word_equal(name, name_length, "visibility")) {
        value = skip_separators(value);
        s->invisible = strncmp(value, "hidden", 6) == 0
                    || strncmp(value, "collapse", 8) == 0;
    }
}

/* Applies "name: value; ..." declarations. */
static void apply_declarations(struct state *s, const char *text,
                               size_t length, const struct render *r) {
    size_t i = 0;
    while (i < length) {
        while (i < length && (text[i] == ' ' || text[i] == ';'
                              || text[i] == '\n' || text[i] == '\t'
                              || text[i] == '\r')) {
            i++;
        }
        size_t name_start = i;
        while (i < length && text[i] != ':' && text[i] != ';') {
            i++;
        }
        if (i >= length || text[i] != ':') {
            continue;
        }
        size_t name_end = i;
        while (name_end > name_start && text[name_end - 1] == ' ') {
            name_end--;
        }
        i++;
        size_t value_start = i;
        int paren = 0;
        while (i < length && (text[i] != ';' || paren > 0)) {
            if (text[i] == '(') paren++;
            if (text[i] == ')' && paren > 0) paren--;
            i++;
        }
        size_t value_end = i;
        char value[256];
        size_t n = value_end - value_start;
        if (n >= sizeof(value)) {
            n = sizeof(value) - 1;
        }
        memcpy(value, text + value_start, n);
        value[n] = '\0';
        char *important = strstr(value, "!important");
        if (important != NULL) {
            *important = '\0';
        }
        apply_property(s, text + name_start, name_end - name_start, value, r);
    }
}

static bool has_class(const struct opos_html_node *node, const char *name,
                      size_t length) {
    const char *classes = opos_html_attribute(node, "class");
    if (classes == NULL) {
        return false;
    }
    const char *p = classes;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == '\n') {
            p++;
        }
        const char *start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n') {
            p++;
        }
        if ((size_t)(p - start) == length && memcmp(start, name, length) == 0) {
            return true;
        }
    }
    return false;
}

static const char *PROPERTIES[] = {
    "fill", "stroke", "fill-opacity", "stroke-opacity", "opacity",
    "stroke-width", "fill-rule", "stroke-linecap", "stroke-linejoin",
    "stroke-miterlimit", "color", "display", "visibility", NULL
};

/* The element's own state from its parent's. */
static void compute_state(struct state *s, const struct state *parent,
                          const struct opos_html_node *node,
                          const struct render *r) {
    *s = *parent;
    s->opacity = parent->opacity;
    s->hidden = false;
    for (int i = 0; PROPERTIES[i] != NULL; i++) {
        const char *value = opos_html_attribute(node, PROPERTIES[i]);
        if (value != NULL) {
            apply_property(s, PROPERTIES[i], strlen(PROPERTIES[i]), value, r);
        }
    }
    const char *id = opos_html_attribute(node, "id");
    for (int i = 0; i < r->rule_count; i++) {
        const struct rule *rule = &r->rules[i];
        bool match = rule->kind == '.'
                   ? has_class(node, rule->name, rule->name_length)
                   : rule->kind == '#'
                   ? id != NULL && strlen(id) == rule->name_length
                     && memcmp(id, rule->name, rule->name_length) == 0
                   : node->name != NULL
                     && word_equal(rule->name, rule->name_length, node->name);
        if (match) {
            apply_declarations(s, rule->declarations,
                               rule->declarations_length, r);
        }
    }
    const char *style = opos_html_attribute(node, "style");
    if (style != NULL) {
        apply_declarations(s, style, strlen(style), r);
    }
}

/* Reads simple rules from the <style> elements under node: selectors that
   are one class, id or element name. Others are skipped. */
static void collect_rules(struct render *r, const struct opos_html_node *node,
                          int depth) {
    if (depth > SVG_MAX_DEPTH) {
        return;
    }
    for (const struct opos_html_node *child = node->first_child;
         child != NULL; child = child->next_sibling) {
        if (child->type != HTML_NODE_ELEMENT) {
            continue;
        }
        if (child->tag != HTML_TAG_STYLE) {
            collect_rules(r, child, depth + 1);
            continue;
        }
        const struct opos_html_node *text = child->first_child;
        if (text == NULL || text->type != HTML_NODE_TEXT || r->style_text) {
            continue; /* only the first <style> */
        }
        char *copy = malloc(text->text_length + 1);
        if (copy == NULL) {
            continue;
        }
        memcpy(copy, text->text, text->text_length);
        copy[text->text_length] = '\0';
        r->style_text = copy;
        char *p = copy;
        while (*p != '\0' && r->rule_count < SVG_MAX_RULES) {
            while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') {
                p++;
            }
            if (strncmp(p, "<![CDATA[", 9) == 0) {
                p += 9;
                continue;
            }
            if (strncmp(p, "/*", 2) == 0) {
                char *end = strstr(p + 2, "*/");
                p = end != NULL ? end + 2 : p + strlen(p);
                continue;
            }
            char *open = strchr(p, '{');
            if (open == NULL) {
                break;
            }
            char *close = strchr(open, '}');
            if (close == NULL) {
                break;
            }
            if (*p == '@') {
                /* @media and the like: skip the whole block. */
                int level = 0;
                char *q = open;
                for (; *q != '\0'; q++) {
                    if (*q == '{') level++;
                    if (*q == '}' && --level == 0) break;
                }
                p = *q != '\0' ? q + 1 : q;
                continue;
            }
            /* Each comma-separated selector. */
            char *selector = p;
            while (selector < open && r->rule_count < SVG_MAX_RULES) {
                while (selector < open && (*selector == ' ' || *selector == ','
                                           || *selector == '\n'
                                           || *selector == '\t'
                                           || *selector == '\r')) {
                    selector++;
                }
                char *end = selector;
                while (end < open && *end != ',') {
                    end++;
                }
                char *last = end;
                while (last > selector && (last[-1] == ' ' || last[-1] == '\n'
                                           || last[-1] == '\t'
                                           || last[-1] == '\r')) {
                    last--;
                }
                size_t n = (size_t)(last - selector);
                bool simple = n > 0;
                for (size_t i = (*selector == '.' || *selector == '#') ? 1 : 0;
                     i < n && simple; i++) {
                    char ch = selector[i];
                    simple = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
                          || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
                }
                if (simple) {
                    struct rule *rule = &r->rules[r->rule_count++];
                    rule->kind = *selector == '.' || *selector == '#'
                               ? *selector : 'e';
                    rule->name = rule->kind == 'e' ? selector : selector + 1;
                    rule->name_length = rule->kind == 'e' ? n : n - 1;
                    rule->declarations = open + 1;
                    rule->declarations_length = (size_t)(close - open - 1);
                }
                selector = end;
            }
            p = close + 1;
        }
    }
}

/* ------------------------------------------------------------- id lookup */

static const struct opos_html_node *find_in(const struct opos_html_node *node,
                                            const char *id, int depth) {
    if (depth > 64) {
        return NULL;
    }
    for (const struct opos_html_node *child = node->first_child;
         child != NULL; child = child->next_sibling) {
        if (child->type != HTML_NODE_ELEMENT) {
            continue;
        }
        const char *value = opos_html_attribute(child, "id");
        if (value != NULL && strcmp(value, id) == 0) {
            return child;
        }
        const struct opos_html_node *found = find_in(child, id, depth + 1);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

/* The element with that id: first under root, then in the whole document
   (pages keep icon <symbol>s in one hidden <svg>). */
const struct opos_html_node *svg_find_id(const struct opos_html_node *root,
                                         const struct opos_html_node *document,
                                         const char *id) {
    if (id == NULL || id[0] == '\0') {
        return NULL;
    }
    const struct opos_html_node *found = find_in(root, id, 0);
    if (found == NULL && document != NULL && document != root) {
        found = find_in(document, id, 0);
    }
    return found;
}

/* An attribute of a gradient, or of the gradients it refers to (href). */
static const char *gradient_attribute(const struct render *r,
                                      const struct opos_html_node *gradient,
                                      const char *name) {
    for (int i = 0; i < 4 && gradient != NULL; i++) {
        const char *value = opos_html_attribute(gradient, name);
        if (value != NULL) {
            return value;
        }
        const char *href = opos_html_attribute(gradient, "href");
        if (href == NULL) {
            href = opos_html_attribute(gradient, "xlink:href");
        }
        if (href == NULL || href[0] != '#') {
            return NULL;
        }
        gradient = svg_find_id(r->root, r->document, href + 1);
    }
    return NULL;
}

/* -------------------------------------------------------------- gradient */

struct gradient {
    bool radial;
    struct matrix to_gradient;   /* device pixel -> gradient units */
    double x1, y1, x2, y2;       /* linear */
    double cx, cy, radius;       /* radial */
    int stop_count;
    float offset[SVG_MAX_STOPS];
    float rgba[SVG_MAX_STOPS][4]; /* premultiplied */
};

static double gradient_number(const struct render *r,
                              const struct opos_html_node *node,
                              const char *name, double fallback,
                              bool bounding_box, double percent_of) {
    const char *value = gradient_attribute(r, node, name);
    double out;
    if (value == NULL) {
        return fallback;
    }
    const char *p = skip_separators(value);
    if (bounding_box) {
        double number;
        if (!read_number(&p, &number)) {
            return fallback;
        }
        return *p == '%' ? number / 100 : number;
    }
    return read_length(value, percent_of, &out) ? out : fallback;
}

static bool gradient_setup(struct render *r, const struct paint *paint,
                           const struct state *s, const struct matrix *m,
                           struct gradient *g) {
    const struct opos_html_node *node = paint->gradient;
    memset(g, 0, sizeof(*g));
    g->radial = strcmp(node->name, "radialgradient") == 0;
    const char *units = gradient_attribute(r, node, "gradientunits");
    bool box = units == NULL
            || !word_equal(units, strlen(units), "userspaceonuse");
    struct matrix space = *m;
    if (box) {
        if (!r->path.have_box) {
            return false;
        }
        double w = r->path.max_x - r->path.min_x;
        double h = r->path.max_y - r->path.min_y;
        if (w <= 0 || h <= 0) {
            return false;
        }
        struct matrix b = {w, 0, 0, h, r->path.min_x, r->path.min_y};
        space = multiply(space, b);
    }
    space = multiply(space,
                     parse_transform(gradient_attribute(r, node,
                                                        "gradienttransform")));
    if (!invert(space, &g->to_gradient)) {
        return false;
    }
    double vw = r->view_width, vh = r->view_height;
    double diagonal = svg_sqrt((vw * vw + vh * vh) / 2);
    if (g->radial) {
        g->cx = gradient_number(r, node, "cx", box ? 0.5 : vw / 2, box, vw);
        g->cy = gradient_number(r, node, "cy", box ? 0.5 : vh / 2, box, vh);
        g->radius = gradient_number(r, node, "r", box ? 0.5 : diagonal / 2,
                                    box, diagonal);
        if (g->radius <= 0) {
            return false;
        }
    } else {
        g->x1 = gradient_number(r, node, "x1", 0, box, vw);
        g->y1 = gradient_number(r, node, "y1", 0, box, vh);
        g->x2 = gradient_number(r, node, "x2", box ? 1 : vw, box, vw);
        g->y2 = gradient_number(r, node, "y2", 0, box, vh);
    }
    /* Stops: from the first gradient in the href chain that has any. */
    const struct opos_html_node *holder = node;
    for (int i = 0; i < 4 && holder != NULL; i++) {
        bool any = false;
        for (const struct opos_html_node *c = holder->first_child; c != NULL;
             c = c->next_sibling) {
            if (c->type == HTML_NODE_ELEMENT && c->name != NULL
                && strcmp(c->name, "stop") == 0) {
                any = true;
                break;
            }
        }
        if (any) {
            break;
        }
        const char *href = opos_html_attribute(holder, "href");
        if (href == NULL) {
            href = opos_html_attribute(holder, "xlink:href");
        }
        holder = href != NULL && href[0] == '#'
               ? svg_find_id(r->root, r->document, href + 1) : NULL;
    }
    if (holder == NULL) {
        return false;
    }
    float previous = 0;
    for (const struct opos_html_node *c = holder->first_child;
         c != NULL && g->stop_count < SVG_MAX_STOPS; c = c->next_sibling) {
        if (c->type != HTML_NODE_ELEMENT || c->name == NULL
            || strcmp(c->name, "stop") != 0) {
            continue;
        }
        float offset = parse_opacity(opos_html_attribute(c, "offset")
                                         ? opos_html_attribute(c, "offset")
                                         : "0", 0);
        if (offset < previous) {
            offset = previous;
        }
        previous = offset;
        /* stop-color and stop-opacity: attribute, then style="". */
        uint32_t colour = 0;
        int alpha = 255;
        float opacity = 1;
        const char *value = opos_html_attribute(c, "stop-color");
        if (value != NULL) {
            if (word_equal(value, strlen(value), "currentcolor")) {
                colour = s->colour;
            } else {
                parse_colour_alpha(value, &colour, &alpha);
            }
        }
        value = opos_html_attribute(c, "stop-opacity");
        if (value != NULL) {
            opacity = parse_opacity(value, 1);
        }
        const char *style = opos_html_attribute(c, "style");
        if (style != NULL) {
            const char *at = strstr(style, "stop-color");
            if (at != NULL && (at = strchr(at, ':')) != NULL) {
                char text[64];
                size_t n = strcspn(at + 1, ";");
                if (n >= sizeof(text)) n = sizeof(text) - 1;
                memcpy(text, at + 1, n);
                text[n] = '\0';
                parse_colour_alpha(text, &colour, &alpha);
            }
            at = strstr(style, "stop-opacity");
            if (at != NULL && (at = strchr(at, ':')) != NULL) {
                opacity = parse_opacity(at + 1, 1);
            }
        }
        float a = alpha / 255.0f * opacity;
        g->offset[g->stop_count] = offset;
        g->rgba[g->stop_count][0] = ((colour >> 16) & 255) / 255.0f * a;
        g->rgba[g->stop_count][1] = ((colour >> 8) & 255) / 255.0f * a;
        g->rgba[g->stop_count][2] = (colour & 255) / 255.0f * a;
        g->rgba[g->stop_count][3] = a;
        g->stop_count++;
    }
    return g->stop_count > 0;
}

static void gradient_at(const struct gradient *g, double x, double y,
                        float out[4]) {
    double gx, gy;
    apply(&g->to_gradient, x, y, &gx, &gy);
    double t;
    if (g->radial) {
        double dx = gx - g->cx, dy = gy - g->cy;
        t = svg_sqrt(dx * dx + dy * dy) / g->radius;
    } else {
        double vx = g->x2 - g->x1, vy = g->y2 - g->y1;
        double length = vx * vx + vy * vy;
        t = length > 0 ? ((gx - g->x1) * vx + (gy - g->y1) * vy) / length : 0;
    }
    int last = g->stop_count - 1;
    if (t <= g->offset[0]) {
        memcpy(out, g->rgba[0], sizeof(float) * 4);
        return;
    }
    if (t >= g->offset[last]) {
        memcpy(out, g->rgba[last], sizeof(float) * 4);
        return;
    }
    for (int i = 0; i < last; i++) {
        if (t <= g->offset[i + 1]) {
            float span = g->offset[i + 1] - g->offset[i];
            float f = span > 0 ? (float)((t - g->offset[i]) / span) : 1;
            for (int k = 0; k < 4; k++) {
                out[k] = g->rgba[i][k] + (g->rgba[i + 1][k] - g->rgba[i][k]) * f;
            }
            return;
        }
    }
    memcpy(out, g->rgba[last], sizeof(float) * 4);
}

/* Turns the accumulated area into coverage and paints it, then clears
   the accumulation rows. */
static void composite(struct render *r, const struct paint *paint,
                      const struct state *s, float opacity, bool even_odd,
                      const struct matrix *m) {
    struct raster *ra = &r->raster;
    int stride = ra->width + 2;
    if (ra->top >= ra->bottom) {
        return;
    }
    float colour[4] = {0};
    struct gradient gradient;
    bool use_gradient = false;
    if (paint->kind == PAINT_GRADIENT) {
        use_gradient = gradient_setup(r, paint, s, m, &gradient);
        if (!use_gradient && paint->has_fallback) {
            uint32_t c = paint->fallback;
            colour[0] = ((c >> 16) & 255) / 255.0f;
            colour[1] = ((c >> 8) & 255) / 255.0f;
            colour[2] = (c & 255) / 255.0f;
            colour[3] = 1;
        }
    } else if (paint->kind == PAINT_COLOUR) {
        uint32_t c = paint->current ? s->colour : paint->colour;
        float a = paint->alpha;
        colour[0] = ((c >> 16) & 255) / 255.0f * a;
        colour[1] = ((c >> 8) & 255) / 255.0f * a;
        colour[2] = (c & 255) / 255.0f * a;
        colour[3] = a;
    }
    bool draw = use_gradient || colour[3] > 0;
    for (int y = ra->top; y < ra->bottom; y++) {
        float *row = ra->cover + (size_t)y * stride;
        float *target = ra->pixels + (size_t)y * ra->width * 4;
        float sum = 0;
        for (int x = 0; x < ra->width; x++) {
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
            if (!draw || cover < 1.0f / 512) {
                continue;
            }
            float source[4];
            if (use_gradient) {
                gradient_at(&gradient, x + 0.5, y + 0.5, source);
            } else {
                memcpy(source, colour, sizeof(source));
            }
            float k = cover * opacity;
            float *p = target + 4 * x;
            float keep = 1 - source[3] * k;
            p[0] = source[0] * k + p[0] * keep;
            p[1] = source[1] * k + p[1] * keep;
            p[2] = source[2] * k + p[2] * keep;
            p[3] = source[3] * k + p[3] * keep;
        }
        row[ra->width] = 0;
        row[ra->width + 1] = 0;
    }
    ra->top = ra->height;
    ra->bottom = 0;
}

/* ---------------------------------------------------------------- stroke */

/* Draws a polygon so that every stroke piece counts positively. */
static void stroke_polygon(struct raster *ra, const double *xy, int n) {
    double area = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += xy[2 * i] * xy[2 * j + 1] - xy[2 * j] * xy[2 * i + 1];
    }
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        if (area >= 0) {
            raster_line(ra, xy[2 * i], xy[2 * i + 1], xy[2 * j],
                        xy[2 * j + 1]);
        } else {
            raster_line(ra, xy[2 * j], xy[2 * j + 1], xy[2 * i],
                        xy[2 * i + 1]);
        }
    }
}

static void stroke_circle(struct raster *ra, double cx, double cy,
                          double radius) {
    double xy[2 * 64];
    int n = (int)(radius * 2) + 8;
    if (n > 64) {
        n = 64;
    }
    for (int i = 0; i < n; i++) {
        double a = 2 * SVG_PI * i / n;
        xy[2 * i] = cx + radius * svg_cos(a);
        xy[2 * i + 1] = cy + radius * svg_sin(a);
    }
    stroke_polygon(ra, xy, n);
}

static void stroke_join(struct raster *ra, const struct state *s, double px,
                        double py, double d0x, double d0y, double d1x,
                        double d1y, double half) {
    if (s->join == 1) {
        stroke_circle(ra, px, py, half);
        return;
    }
    double cross = d0x * d1y - d0y * d1x;
    if (svg_fabs(cross) < 1e-9 && d0x * d1x + d0y * d1y > 0) {
        return; /* straight on */
    }
    double side = cross > 0 ? -1 : 1;
    double n0x = -d0y * half * side, n0y = d0x * half * side;
    double n1x = -d1y * half * side, n1y = d1x * half * side;
    if (s->join == 0) {
        double mx = n0x + n1x, my = n0y + n1y;
        double ml = svg_sqrt(mx * mx + my * my);
        if (ml > 1e-9) {
            mx /= ml;
            my /= ml;
            double dot = (mx * n0x + my * n0y) / half;
            if (dot > 1e-9 && 1 / dot <= s->miter_limit) {
                double length = half / dot;
                double xy[8] = {px, py, px + n0x, py + n0y,
                                px + mx * length, py + my * length,
                                px + n1x, py + n1y};
                stroke_polygon(ra, xy, 4);
                return;
            }
        }
    }
    double xy[6] = {px, py, px + n0x, py + n0y, px + n1x, py + n1y};
    stroke_polygon(ra, xy, 3);
}

static void stroke_path(struct raster *ra, const struct path *path,
                        const struct state *s, double half) {
    for (int sp = 0; sp < path->subpaths; sp++) {
        int start = path->starts[sp], end = subpath_end(path, sp);
        bool closed = path->closed[sp];
        /* Drop repeated points; they have no direction. */
        int n = end - start;
        if (n <= 0) {
            continue;
        }
        const double *xy = path->xy;
        int unique = 0;
        double previous_x = 0, previous_y = 0;
        bool have = false;
        double first_dx = 0, first_dy = 0, last_dx = 0, last_dy = 0;
        double start_x = xy[2 * start], start_y = xy[2 * start + 1];
        double end_x = start_x, end_y = start_y;
        int total = closed ? n + 1 : n;
        for (int k = 0; k < total; k++) {
            int i = start + (k % n);
            double x = xy[2 * i], y = xy[2 * i + 1];
            if (have) {
                double dx = x - previous_x, dy = y - previous_y;
                double length = svg_sqrt(dx * dx + dy * dy);
                if (length < 1e-6) {
                    continue;
                }
                dx /= length;
                dy /= length;
                double nx = -dy * half, ny = dx * half;
                double quad[8] = {previous_x + nx, previous_y + ny,
                                  x + nx, y + ny, x - nx, y - ny,
                                  previous_x - nx, previous_y - ny};
                stroke_polygon(ra, quad, 4);
                if (unique > 0) {
                    stroke_join(ra, s, previous_x, previous_y, last_dx,
                                last_dy, dx, dy, half);
                } else {
                    first_dx = dx;
                    first_dy = dy;
                }
                last_dx = dx;
                last_dy = dy;
                unique++;
                end_x = x;
                end_y = y;
            }
            previous_x = x;
            previous_y = y;
            have = true;
        }
        if (unique == 0) {
            /* A zero-length subpath shows only with round or square caps. */
            if (s->cap == 1) {
                stroke_circle(ra, start_x, start_y, half);
            } else if (s->cap == 2) {
                double sq[8] = {start_x - half, start_y - half,
                                start_x + half, start_y - half,
                                start_x + half, start_y + half,
                                start_x - half, start_y + half};
                stroke_polygon(ra, sq, 4);
            }
            continue;
        }
        if (closed) {
            stroke_join(ra, s, start_x, start_y, last_dx, last_dy, first_dx,
                        first_dy, half);
            continue;
        }
        for (int e = 0; e < 2; e++) {
            double x = e == 0 ? start_x : end_x, y = e == 0 ? start_y : end_y;
            double dx = e == 0 ? -first_dx : last_dx;
            double dy = e == 0 ? -first_dy : last_dy;
            if (s->cap == 1) {
                stroke_circle(ra, x, y, half);
            } else if (s->cap == 2) {
                double nx = -dy * half, ny = dx * half;
                double sq[8] = {x + nx, y + ny, x + nx + dx * half,
                                y + ny + dy * half, x - nx + dx * half,
                                y - ny + dy * half, x - nx, y - ny};
                stroke_polygon(ra, sq, 4);
            }
        }
    }
}

/* ------------------------------------------------------------- elements */

static void draw_path(struct render *r, const struct state *s,
                      const struct matrix *m) {
    struct path *path = &r->path;
    if (path->failed || path->count < 2 || s->invisible) {
        return;
    }
    if (s->fill.kind != PAINT_NONE) {
        for (int sp = 0; sp < path->subpaths; sp++) {
            int start = path->starts[sp], end = subpath_end(path, sp);
            for (int i = start; i < end; i++) {
                int j = i + 1 < end ? i + 1 : start;
                raster_line(&r->raster, path->xy[2 * i], path->xy[2 * i + 1],
                            path->xy[2 * j], path->xy[2 * j + 1]);
            }
        }
        composite(r, &s->fill, s, s->fill_opacity * s->opacity, s->even_odd,
                  m);
    }
    if (s->stroke.kind != PAINT_NONE && s->stroke_width > 0) {
        double scale = svg_sqrt(svg_fabs(m->a * m->d - m->b * m->c));
        double half = s->stroke_width * scale / 2;
        float opacity = s->stroke_opacity * s->opacity;
        if (half < 0.35) {
            /* Hairlines: a thinner line drawn fainter looks right. */
            opacity *= (float)(half / 0.35);
            half = 0.35;
        }
        stroke_path(&r->raster, path, s, half);
        composite(r, &s->stroke, s, opacity, false, m);
    }
}

static double attribute_length(const struct opos_html_node *node,
                               const char *name, double percent_of,
                               double fallback) {
    double value;
    return read_length(opos_html_attribute(node, name), percent_of, &value)
           ? value : fallback;
}

static void build_ellipse(struct builder *b, double cx, double cy, double rx,
                          double ry) {
    const double k = 0.5522847498;
    build_move(b, cx + rx, cy);
    build_cubic(b, cx + rx, cy + ry * k, cx + rx * k, cy + ry, cx, cy + ry);
    build_cubic(b, cx - rx * k, cy + ry, cx - rx, cy + ry * k, cx - rx, cy);
    build_cubic(b, cx - rx, cy - ry * k, cx - rx * k, cy - ry, cx, cy - ry);
    build_cubic(b, cx + rx * k, cy - ry, cx + rx, cy - ry * k, cx + rx, cy);
    build_close(b);
}

/* Fills the path for a basic shape; false when it has nothing to draw. */
static bool build_shape(struct render *r, const struct opos_html_node *node,
                        const struct matrix *m) {
    struct builder b = {&r->path, *m, 0, 0, 0, 0, false};
    path_reset(&r->path);
    const char *name = node->name;
    double vw = r->view_width, vh = r->view_height;
    double diagonal = svg_sqrt((vw * vw + vh * vh) / 2);
    if (strcmp(name, "path") == 0) {
        build_path_data(&b, opos_html_attribute(node, "d"));
    } else if (strcmp(name, "rect") == 0) {
        double x = attribute_length(node, "x", vw, 0);
        double y = attribute_length(node, "y", vh, 0);
        double w = attribute_length(node, "width", vw, 0);
        double h = attribute_length(node, "height", vh, 0);
        if (w <= 0 || h <= 0) {
            return false;
        }
        double rx = attribute_length(node, "rx", vw, -1);
        double ry = attribute_length(node, "ry", vh, -1);
        if (rx < 0) rx = ry;
        if (ry < 0) ry = rx;
        if (rx < 0) rx = ry = 0;
        if (rx > w / 2) rx = w / 2;
        if (ry > h / 2) ry = h / 2;
        if (rx <= 0 || ry <= 0) {
            build_move(&b, x, y);
            build_line(&b, x + w, y);
            build_line(&b, x + w, y + h);
            build_line(&b, x, y + h);
            build_close(&b);
        } else {
            const double k = 0.5522847498;
            build_move(&b, x + rx, y);
            build_line(&b, x + w - rx, y);
            build_cubic(&b, x + w - rx + rx * k, y, x + w, y + ry - ry * k,
                        x + w, y + ry);
            build_line(&b, x + w, y + h - ry);
            build_cubic(&b, x + w, y + h - ry + ry * k, x + w - rx + rx * k,
                        y + h, x + w - rx, y + h);
            build_line(&b, x + rx, y + h);
            build_cubic(&b, x + rx - rx * k, y + h, x, y + h - ry + ry * k, x,
                        y + h - ry);
            build_line(&b, x, y + ry);
            build_cubic(&b, x, y + ry - ry * k, x + rx - rx * k, y, x + rx, y);
            build_close(&b);
        }
    } else if (strcmp(name, "circle") == 0) {
        double radius = attribute_length(node, "r", diagonal, 0);
        if (radius <= 0) {
            return false;
        }
        build_ellipse(&b, attribute_length(node, "cx", vw, 0),
                      attribute_length(node, "cy", vh, 0), radius, radius);
    } else if (strcmp(name, "ellipse") == 0) {
        double rx = attribute_length(node, "rx", vw, -1);
        double ry = attribute_length(node, "ry", vh, -1);
        if (rx < 0) rx = ry;
        if (ry < 0) ry = rx;
        if (rx <= 0 || ry <= 0) {
            return false;
        }
        build_ellipse(&b, attribute_length(node, "cx", vw, 0),
                      attribute_length(node, "cy", vh, 0), rx, ry);
    } else if (strcmp(name, "line") == 0) {
        build_move(&b, attribute_length(node, "x1", vw, 0),
                   attribute_length(node, "y1", vh, 0));
        build_line(&b, attribute_length(node, "x2", vw, 0),
                   attribute_length(node, "y2", vh, 0));
    } else if (strcmp(name, "polyline") == 0
               || strcmp(name, "polygon") == 0) {
        const char *p = opos_html_attribute(node, "points");
        double x, y;
        bool first = true;
        while (p != NULL && read_number(&p, &x) && read_number(&p, &y)) {
            if (first) {
                build_move(&b, x, y);
                first = false;
            } else {
                build_line(&b, x, y);
            }
        }
        if (name[4] == 'g') {
            build_close(&b);
        }
    } else {
        return false;
    }
    return r->path.count > 1;
}

/* The transform from a viewport (x, y, width, height in the parent's
   units) to the content of an element with viewBox and
   preserveAspectRatio. */
static struct matrix viewport_matrix(const struct opos_html_node *node,
                                     double x, double y, double width,
                                     double height, double *view_width,
                                     double *view_height) {
    struct matrix t = {1, 0, 0, 1, x, y};
    *view_width = width;
    *view_height = height;
    const char *box = opos_html_attribute(node, "viewbox");
    double v[4];
    const char *p = box;
    if (box == NULL || !read_number(&p, &v[0]) || !read_number(&p, &v[1])
        || !read_number(&p, &v[2]) || !read_number(&p, &v[3]) || v[2] <= 0
        || v[3] <= 0) {
        return t;
    }
    *view_width = v[2];
    *view_height = v[3];
    double sx = width / v[2], sy = height / v[3];
    const char *aspect = opos_html_attribute(node, "preserveaspectratio");
    double ax = 0.5, ay = 0.5;
    bool slice = false, none = false;
    if (aspect != NULL) {
        none = strstr(aspect, "none") != NULL;
        slice = strstr(aspect, "slice") != NULL;
        if (strstr(aspect, "xMin") != NULL || strstr(aspect, "xmin") != NULL) ax = 0;
        if (strstr(aspect, "xMax") != NULL || strstr(aspect, "xmax") != NULL) ax = 1;
        if (strstr(aspect, "YMin") != NULL || strstr(aspect, "ymin") != NULL) ay = 0;
        if (strstr(aspect, "YMax") != NULL || strstr(aspect, "ymax") != NULL) ay = 1;
    }
    if (!none) {
        double s = slice ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
        sx = sy = s;
    }
    t.a = sx;
    t.d = sy;
    t.e = x + (width - v[2] * sx) * ax - v[0] * sx;
    t.f = y + (height - v[3] * sy) * ay - v[1] * sy;
    return t;
}

static void render_children(struct render *r,
                            const struct opos_html_node *node,
                            const struct state *s, const struct matrix *m);

static void render_node(struct render *r, const struct opos_html_node *node,
                        const struct state *parent, const struct matrix *pm) {
    if (node->type != HTML_NODE_ELEMENT || node->name == NULL
        || r->depth > SVG_MAX_DEPTH) {
        return;
    }
    const char *name = node->name;
    /* Elements that are never drawn where they stand. */
    static const char *const SKIP[] = {
        "defs", "symbol", "clippath", "mask", "pattern", "lineargradient",
        "radialgradient", "style", "title", "desc", "metadata", "marker",
        "filter", "script", "foreignobject", "image", "text", NULL
    };
    for (int i = 0; SKIP[i] != NULL; i++) {
        if (strcmp(name, SKIP[i]) == 0) {
            return;
        }
    }
    struct state s;
    compute_state(&s, parent, node, r);
    if (s.hidden) {
        return;
    }
    struct matrix m = multiply(*pm, parse_transform(
                                        opos_html_attribute(node, "transform")));
    r->depth++;
    if (strcmp(name, "g") == 0 || strcmp(name, "a") == 0) {
        render_children(r, node, &s, &m);
    } else if (strcmp(name, "switch") == 0) {
        for (const struct opos_html_node *c = node->first_child; c != NULL;
             c = c->next_sibling) {
            if (c->type == HTML_NODE_ELEMENT) {
                render_node(r, c, &s, &m);
                break;
            }
        }
    } else if (strcmp(name, "svg") == 0) {
        double vw = r->view_width, vh = r->view_height;
        double x = attribute_length(node, "x", vw, 0);
        double y = attribute_length(node, "y", vh, 0);
        double w = attribute_length(node, "width", vw, vw);
        double h = attribute_length(node, "height", vh, vh);
        double nvw, nvh;
        struct matrix inner = multiply(m, viewport_matrix(node, x, y, w, h,
                                                          &nvw, &nvh));
        r->view_width = nvw;
        r->view_height = nvh;
        render_children(r, node, &s, &inner);
        r->view_width = vw;
        r->view_height = vh;
    } else if (strcmp(name, "use") == 0) {
        const char *href = opos_html_attribute(node, "href");
        if (href == NULL) {
            href = opos_html_attribute(node, "xlink:href");
        }
        const struct opos_html_node *target =
            href != NULL && href[0] == '#'
                ? svg_find_id(r->root, r->document, href + 1) : NULL;
        bool loop = false;
        for (const struct opos_html_node *a = node; a != NULL; a = a->parent) {
            if (a == target) {
                loop = true;
            }
        }
        if (target != NULL && !loop && target->name != NULL) {
            double vw = r->view_width, vh = r->view_height;
            double x = attribute_length(node, "x", vw, 0);
            double y = attribute_length(node, "y", vh, 0);
            struct matrix moved = multiply(m, (struct matrix){1, 0, 0, 1, x, y});
            if (strcmp(target->name, "symbol") == 0
                || strcmp(target->name, "svg") == 0) {
                double w = attribute_length(node, "width", vw,
                               attribute_length(target, "width", vw, vw));
                double h = attribute_length(node, "height", vh,
                               attribute_length(target, "height", vh, vh));
                double nvw, nvh;
                struct matrix inner = multiply(
                    moved, viewport_matrix(target, 0, 0, w, h, &nvw, &nvh));
                struct state ts;
                compute_state(&ts, &s, target, r);
                r->view_width = nvw;
                r->view_height = nvh;
                if (!ts.hidden || strcmp(target->name, "symbol") == 0) {
                    ts.hidden = false;
                    render_children(r, target, &ts, &inner);
                }
                r->view_width = vw;
                r->view_height = vh;
            } else {
                render_node(r, target, &s, &moved);
            }
        }
    } else if (build_shape(r, node, &m)) {
        draw_path(r, &s, &m);
    }
    r->depth--;
}

static void render_children(struct render *r,
                            const struct opos_html_node *node,
                            const struct state *s, const struct matrix *m) {
    for (const struct opos_html_node *child = node->first_child;
         child != NULL; child = child->next_sibling) {
        render_node(r, child, s, m);
    }
}

/* ---------------------------------------------------------------- public */

const struct opos_html_node *svg_root(const struct opos_html_node *node) {
    if (node == NULL) {
        return NULL;
    }
    if (node->type == HTML_NODE_ELEMENT && node->tag == HTML_TAG_SVG) {
        return node;
    }
    for (const struct opos_html_node *child = node->first_child;
         child != NULL; child = child->next_sibling) {
        const struct opos_html_node *found = svg_root(child);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

bool svg_size(const struct opos_html_node *svg, int *width, int *height) {
    double w = -1, h = -1;
    const char *text = opos_html_attribute(svg, "width");
    if (text != NULL && strchr(text, '%') == NULL) {
        read_length(text, 0, &w);
    }
    text = opos_html_attribute(svg, "height");
    if (text != NULL && strchr(text, '%') == NULL) {
        read_length(text, 0, &h);
    }
    double v[4];
    const char *p = opos_html_attribute(svg, "viewbox");
    bool box = p != NULL && read_number(&p, &v[0]) && read_number(&p, &v[1])
            && read_number(&p, &v[2]) && read_number(&p, &v[3]) && v[2] > 0
            && v[3] > 0;
    if (w <= 0 && h <= 0) {
        if (box) {
            w = v[2];
            h = v[3];
        } else {
            w = 300;
            h = 150;
        }
    } else if (w <= 0) {
        w = box ? h * v[2] / v[3] : 300;
    } else if (h <= 0) {
        h = box ? w * v[3] / v[2] : 150;
    }
    *width = (int)(w + 0.5);
    *height = (int)(h + 0.5);
    if (*width < 1) *width = 1;
    if (*height < 1) *height = 1;
    return true;
}

uint32_t *svg_render(const struct opos_html_node *svg, int width, int height,
                     uint32_t colour) {
    if (svg == NULL || width <= 0 || height <= 0
        || (size_t)width * (size_t)height > 8u * 1024 * 1024) {
        return NULL;
    }
    struct render *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    size_t count = (size_t)width * (size_t)height;
    r->raster.width = width;
    r->raster.height = height;
    r->raster.cover = calloc((size_t)(width + 2) * (size_t)height,
                             sizeof(float));
    r->raster.pixels = calloc(count * 4, sizeof(float));
    r->raster.top = height;
    r->raster.bottom = 0;
    uint32_t *out = malloc(count * 4);
    if (r->raster.cover == NULL || r->raster.pixels == NULL || out == NULL) {
        free(r->raster.cover);
        free(r->raster.pixels);
        free(out);
        free(r);
        return NULL;
    }
    r->root = svg;
    const struct opos_html_node *top = svg;
    while (top->parent != NULL) {
        top = top->parent;
    }
    r->document = top;
    collect_rules(r, svg, 0);

    struct state s;
    memset(&s, 0, sizeof(s));
    s.fill.kind = PAINT_COLOUR;
    s.fill.alpha = 1;
    s.fill.colour = 0;
    s.stroke.kind = PAINT_NONE;
    s.fill_opacity = s.stroke_opacity = s.opacity = 1;
    s.stroke_width = 1;
    s.miter_limit = 4;
    s.colour = colour;

    double vw, vh;
    struct matrix m = viewport_matrix(svg, 0, 0, width, height, &vw, &vh);
    if (opos_html_attribute(svg, "viewbox") == NULL) {
        /* No viewBox: the content is in the svg's own size; scale it. */
        int iw, ih;
        svg_size(svg, &iw, &ih);
        m.a = (double)width / iw;
        m.d = (double)height / ih;
        vw = iw;
        vh = ih;
    }
    r->view_width = vw;
    r->view_height = vh;
    struct state root_state;
    compute_state(&root_state, &s, svg, r);
    if (!root_state.hidden) {
        struct matrix t = multiply(m, parse_transform(
                                          opos_html_attribute(svg, "transform")));
        render_children(r, svg, &root_state, &t);
    }

    for (size_t i = 0; i < count; i++) {
        const float *p = r->raster.pixels + 4 * i;
        float a = p[3];
        if (a <= 1.0f / 255) {
            out[i] = 0;
            continue;
        }
        if (a > 1) {
            a = 1;
        }
        int red = (int)(p[0] / a * 255 + 0.5f);
        int green = (int)(p[1] / a * 255 + 0.5f);
        int blue = (int)(p[2] / a * 255 + 0.5f);
        red = red > 255 ? 255 : red < 0 ? 0 : red;
        green = green > 255 ? 255 : green < 0 ? 0 : green;
        blue = blue > 255 ? 255 : blue < 0 ? 0 : blue;
        out[i] = (uint32_t)(a * 255 + 0.5f) << 24 | (uint32_t)red << 16
               | (uint32_t)green << 8 | (uint32_t)blue;
    }
    path_free(&r->path);
    free(r->style_text);
    free(r->raster.cover);
    free(r->raster.pixels);
    free(r);
    return out;
}
