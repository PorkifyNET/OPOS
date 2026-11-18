/* OPOS SDK C library: floating-point rounding mode (SSE MXCSR). */
#ifndef OPOS_FENV_H
#define OPOS_FENV_H

#define FE_TONEAREST 0x0000
#define FE_DOWNWARD 0x2000
#define FE_UPWARD 0x4000
#define FE_TOWARDZERO 0x6000

int fegetround(void);
int fesetround(int mode);

#endif
