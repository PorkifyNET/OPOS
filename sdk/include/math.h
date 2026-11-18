/*
 * The part of <math.h> that the SDK provides (lib/math.c), enough for
 * common single-file libraries such as audio decoders and the browser's
 * JavaScript engine. Results are accurate to a few units in the last
 * place, not always correctly rounded.
 */
#ifndef OPOS_MATH_H
#define OPOS_MATH_H

#define M_PI 3.14159265358979323846
#define HUGE_VAL __builtin_huge_val()
#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")

#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)

double fabs(double x);
float fabsf(float x);
double floor(double x);
float floorf(float x);
double ceil(double x);
float ceilf(float x);
double fmod(double x, double y);
double sqrt(double x);
float sqrtf(float x);
double ldexp(double x, int exponent);
float ldexpf(float x, int exponent);
double frexp(double x, int *exponent);
double exp(double x);
float expf(float x);
double log(double x);
float logf(float x);
double log10(double x);
double pow(double x, double y);
float powf(float x, float y);
double sin(double x);
float sinf(float x);
double cos(double x);
float cosf(float x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double asinh(double x);
double acosh(double x);
double atanh(double x);
double expm1(double x);
double log1p(double x);
double log2(double x);
double cbrt(double x);
double hypot(double x, double y);
double trunc(double x);
double round(double x);
double rint(double x);
double nearbyint(double x);
long lrint(double x);
double modf(double x, double *whole);
double copysign(double x, double sign);
double fmin(double x, double y);
double fmax(double x, double y);

#endif
