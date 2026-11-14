/* SPDX-License-Identifier: GPL-3.0-or-later
 * A no-entry DLL needs neither CRT startup nor a security-cookie initializer.
 * clang emits this marker for floating-point code. UCRT is part of Windows 10+. */
int _fltused = 0;
/* MSVC's math header implements fabsf as an intrinsic; UCRT has no fabsf export.
 * Explicitly request clang's bitwise intrinsic, preserving signed NaN payloads. */
#undef fabsf                    /* tools/release/include/math.h maps it to the builtin; this is the function itself */
float fabsf(float x) { return __builtin_fabsf(x); }
