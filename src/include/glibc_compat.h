/*
 * glibc compatibility - ensure libamvgpu.so works on glibc 2.35+.
 *
 * _GNU_SOURCE implies _ISOC2X_SOURCE on glibc 2.38+, which causes
 * strtoull/strtol to redirect to __isoc23_* variants. This creates
 * a hard GLIBC_2.38 dependency that breaks in containers with older
 * glibc (e.g., Ubuntu 22.04 rocm/pytorch images use glibc 2.35).
 *
 * Fix: Include <features.h> to process _GNU_SOURCE, then override
 * the ISOC2X flag before any stdlib headers use it for __asm__ renames.
 *
 * The guard macro was renamed from __GLIBC_USE_C2X_STRTOL to
 * __GLIBC_USE_C23_STRTOL when glibc followed the C standard's own
 * rename from the C2x draft to C23 (around glibc 2.40). Override both
 * spellings so this keeps working across the whole 2.34+ range instead
 * of silently doing nothing on newer glibc.
 *
 * IMPORTANT: This header MUST be included BEFORE <stdlib.h>, <stdio.h>,
 * or any other header that uses strtoull/strtol/atoi.
 */

#ifndef GLIBC_COMPAT_H
#define GLIBC_COMPAT_H

/* Force features.h to process _GNU_SOURCE now */
#include <features.h>

/* Override: disable C2X/C23 strtol redirection, whichever name this
 * glibc uses. */
#ifdef __GLIBC_USE_C2X_STRTOL
#undef __GLIBC_USE_C2X_STRTOL
#define __GLIBC_USE_C2X_STRTOL 0
#endif
#ifdef __GLIBC_USE_C23_STRTOL
#undef __GLIBC_USE_C23_STRTOL
#define __GLIBC_USE_C23_STRTOL 0
#endif

#endif /* GLIBC_COMPAT_H */
