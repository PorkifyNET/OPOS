/*
 * OPOS SDK C library: the math functions in <math.h>, and assert().
 *
 * exp and log reduce the argument by powers of two and use short series
 * (exp: Taylor on |r| <= ln2/2; log: 2 atanh(s) on [sqrt(1/2), sqrt(2))).
 * sin and cos reduce by pi/2 in three parts (Cody-Waite), then use
 * Taylor series on |r| <= pi/4. pow with a whole exponent multiplies, so
 * pow(3, 4) is exactly 81; otherwise it is exp(y log x).
 */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <opos.h>

#define LN2_HI 6.93147180369123816490e-01
#define LN2_LO 1.90821492927058770002e-10
#define INV_LN2 1.44269504088896338700e+00
#define LN10 2.30258509299404568402
/* pi/2 in three parts, each exact in a double with the next multiplied
   by a whole number of quarter turns. */
#define PIO2_1 1.57079632673412561417e+00
#define PIO2_2 6.07710050650619224932e-11
#define PIO2_3 2.02226624879595063154e-21
#define INV_PIO2 6.36619772367581382433e-01

union bits {
    double value;
    uint64_t raw;
};

static double from_raw(uint64_t raw) {
    union bits bits = {.raw = raw};
    return bits.value;
}

static uint64_t to_raw(double value) {
    union bits bits = {.value = value};
    return bits.raw;
}

double fabs(double x) {
    return from_raw(to_raw(x) & ~((uint64_t)1 << 63));
}

float fabsf(float x) {
    return (float)fabs(x);
}

double floor(double x) {
    /* From 2^52 up every double is whole; NaN and infinities stay. */
    if (!(fabs(x) < 4503599627370496.0)) {
        return x;
    }
    double whole = (double)(int64_t)x;
    if (whole > x) {
        whole -= 1.0;
    }
    return whole == 0.0 && x < 0.0 ? -0.0 : whole;
}

float floorf(float x) {
    return (float)floor(x);
}

double ceil(double x) {
    return -floor(-x);
}

float ceilf(float x) {
    return (float)ceil(x);
}

double fmod(double x, double y) {
    if (y == 0.0 || isinf(x) || isnan(x) || isnan(y)) {
        return NAN;
    }
    double quotient = x / y;
    double whole = quotient < 0.0 ? -floor(-quotient) : floor(quotient);
    return x - whole * y;
}

double sqrt(double x) {
    double result;
    __asm__("sqrtsd %1, %0" : "=x"(result) : "x"(x));
    return result;
}

float sqrtf(float x) {
    return (float)sqrt(x);
}

double frexp(double x, int *exponent) {
    *exponent = 0;
    if (x == 0.0 || isnan(x) || isinf(x)) {
        return x;
    }
    uint64_t raw = to_raw(x);
    int biased = (int)((raw >> 52) & 0x7ff);
    if (biased == 0) {
        /* Subnormal: scale up into the normal range first. */
        x *= 18014398509481984.0; /* 2^54 */
        raw = to_raw(x);
        biased = (int)((raw >> 52) & 0x7ff);
        *exponent = -54;
    }
    *exponent += biased - 1022;
    raw = (raw & ~((uint64_t)0x7ff << 52)) | ((uint64_t)1022 << 52);
    return from_raw(raw);
}

/* 2^n for -1022 <= n <= 1023. */
static double power_of_two(int n) {
    return from_raw((uint64_t)(n + 1023) << 52);
}

double ldexp(double x, int exponent) {
    while (exponent > 1023) {
        x *= power_of_two(1023);
        exponent -= 1023;
        if (isinf(x)) {
            return x;
        }
    }
    while (exponent < -1022) {
        /* Two steps keep subnormal results from rounding twice much. */
        x *= power_of_two(-511);
        exponent += 511;
        if (x == 0.0) {
            return x;
        }
    }
    return x * power_of_two(exponent);
}

float ldexpf(float x, int exponent) {
    return (float)ldexp(x, exponent);
}

double exp(double x) {
    if (isnan(x)) {
        return x;
    }
    if (x > 709.782712893384) {
        return HUGE_VAL;
    }
    if (x < -745.1332191019412) {
        return 0.0;
    }
    double k = floor(x * INV_LN2 + 0.5);
    double r = (x - k * LN2_HI) - k * LN2_LO;
    /* e^r for |r| <= 0.347: Taylor series to r^13. */
    double term = 1.0;
    double sum = 1.0;
    for (int i = 1; i <= 13; i++) {
        term *= r / i;
        sum += term;
    }
    return ldexp(sum, (int)k);
}

float expf(float x) {
    return (float)exp(x);
}

double log(double x) {
    if (isnan(x) || x < 0.0) {
        return NAN;
    }
    if (x == 0.0) {
        return -HUGE_VAL;
    }
    if (isinf(x)) {
        return x;
    }
    int exponent;
    double m = frexp(x, &exponent); /* [0.5, 1) */
    if (m < 0.70710678118654752440) {
        m *= 2.0;
        exponent--;
    }
    /* log m = 2 atanh(s), s = (m - 1) / (m + 1), |s| <= 0.1716. */
    double s = (m - 1.0) / (m + 1.0);
    double s2 = s * s;
    double power = s;
    double sum = 0.0;
    for (int i = 1; i <= 23; i += 2) {
        sum += power / i;
        power *= s2;
    }
    return exponent * LN2_HI + (2.0 * sum + exponent * LN2_LO);
}

float logf(float x) {
    return (float)log(x);
}

double log10(double x) {
    return log(x) / LN10;
}

double pow(double x, double y) {
    if (y == 0.0 || x == 1.0) {
        return 1.0;
    }
    if (isnan(x) || isnan(y)) {
        return NAN;
    }
    bool whole = fabs(y) < 2147483648.0 && floor(y) == y;
    if (whole) {
        long n = (long)y;
        unsigned long count = (unsigned long)(n < 0 ? -n : n);
        double result = 1.0;
        double base = x;
        while (count != 0) {
            if (count & 1) {
                result *= base;
            }
            base *= base;
            count >>= 1;
        }
        return n < 0 ? 1.0 / result : result;
    }
    if (x < 0.0) {
        return NAN;
    }
    if (x == 0.0) {
        return y > 0.0 ? 0.0 : HUGE_VAL;
    }
    return exp(y * log(x));
}

float powf(float x, float y) {
    return (float)pow(x, y);
}

/* sin and cos of r for |r| <= pi/4: Taylor series to r^17 and r^18. */
static double sin_kernel(double r) {
    double r2 = r * r;
    double term = r;
    double sum = r;
    for (int i = 2; i <= 16; i += 2) {
        term *= -r2 / (i * (i + 1));
        sum += term;
    }
    return sum;
}

static double cos_kernel(double r) {
    double r2 = r * r;
    double term = 1.0;
    double sum = 1.0;
    for (int i = 1; i <= 17; i += 2) {
        term *= -r2 / (i * (i + 1));
        sum += term;
    }
    return sum;
}

/* x = quadrant * pi/2 + r, |r| <= pi/4 (accurate for |x| below ~1e9). */
static double reduce(double x, long *quadrant) {
    double k = floor(x * INV_PIO2 + 0.5);
    *quadrant = (long)k;
    return ((x - k * PIO2_1) - k * PIO2_2) - k * PIO2_3;
}

double sin(double x) {
    if (isnan(x) || isinf(x)) {
        return NAN;
    }
    long quadrant;
    double r = reduce(x, &quadrant);
    switch (quadrant & 3) {
    case 0:
        return sin_kernel(r);
    case 1:
        return cos_kernel(r);
    case 2:
        return -sin_kernel(r);
    default:
        return -cos_kernel(r);
    }
}

float sinf(float x) {
    return (float)sin(x);
}

double cos(double x) {
    if (isnan(x) || isinf(x)) {
        return NAN;
    }
    long quadrant;
    double r = reduce(x, &quadrant);
    switch (quadrant & 3) {
    case 0:
        return cos_kernel(r);
    case 1:
        return -sin_kernel(r);
    case 2:
        return -cos_kernel(r);
    default:
        return sin_kernel(r);
    }
}

float cosf(float x) {
    return (float)cos(x);
}

void opos_assert_failed(const char *expression, const char *file, int line) {
    eprintf("%s:%d: assertion failed: %s\n", file, line, expression);
    opos_exit(134);
}
