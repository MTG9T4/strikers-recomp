/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef STRIKERS_PLAYER_MATH_H
#define STRIKERS_PLAYER_MATH_H
double sqrt(double);
float sqrtf(float);
double fabs(double);
float fabsf(float);
double trunc(double);
float truncf(float);
double floor(double);
float floorf(float);
double ceil(double);
float ceilf(float);
double rint(double);
float rintf(float);
double ldexp(double, int);
double fma(double, double, double);
float fmaf(float, float, float);
/* -ffreestanding turns the compiler's knowledge of these functions off, so every lifted fmadd, fsqrt or frint was a call into
 * ucrtbase. As builtins they are single instructions where the CPU level has them (build_player.py CPU_FLAGS: FMA on v3,
 * ROUNDSS on v2 and v3, SQRTSS everywhere) and the same library call where it does not. The results are the same bits. */
#define sqrt(x) __builtin_sqrt(x)
#define sqrtf(x) __builtin_sqrtf(x)
#define fabs(x) __builtin_fabs(x)
#define fabsf(x) __builtin_fabsf(x)
#define trunc(x) __builtin_trunc(x)
#define truncf(x) __builtin_truncf(x)
#define floor(x) __builtin_floor(x)
#define floorf(x) __builtin_floorf(x)
#define ceil(x) __builtin_ceil(x)
#define ceilf(x) __builtin_ceilf(x)
#define rint(x) __builtin_rint(x)
#define rintf(x) __builtin_rintf(x)
#define fma(a, b, c) __builtin_fma(a, b, c)
#define fmaf(a, b, c) __builtin_fmaf(a, b, c)
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)
#endif
