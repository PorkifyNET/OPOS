/*
 * The part of <math.h> that the SDK provides (lib/math.c), enough for
 * common single-file libraries such as audio decoders. Results are
 * accurate to a few units in the last place, not always correctly
 * rounded.
 */
#ifndef OPOS_MATH_H
#define OPOS_MATH_H

#define M_PI 3.14159265358979323846
#define HUGE_VAL __builtin_huge_val()
#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")

#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)

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

#endif
