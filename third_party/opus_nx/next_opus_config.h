/* Build configuration for libopus in NSDVR (sysmodule and host build use the same file).
 * Force-included into every libopus source file (-include next_opus_config.h), replacing config.h.
 *
 *  - fixed point: smallest encoder state and pseudostack, bit exact across platforms, as fast as
 *    the float build on AArch64 (both use NEON intrinsics); see host/README.md for measurements
 *  - no heap: opus_alloc() is stubbed to return NULL, temporary buffers come from a static
 *    pseudostack owned by next_audio.c (NONTHREADSAFE_PSEUDOSTACK + OVERRIDE_OPUS_ALLOC_SCRATCH)
 *  - ENABLE_HARDENING turns pseudostack overflows / internal assertions into celt_fatal(), which
 *    next_audio.c overrides to longjmp back to the caller instead of abort()
 */
#ifndef NEXT_OPUS_CONFIG_H
#define NEXT_OPUS_CONFIG_H

#include <stddef.h>

#define OPUS_BUILD 1
#define PACKAGE_VERSION "1.6.1"
#define FIXED_POINT 1
#define DISABLE_FLOAT_API 1
#define HAVE_LRINT 1
#define HAVE_LRINTF 1

#define NONTHREADSAFE_PSEUDOSTACK 1
extern int next_opus_pseudostack_limit;
#define GLOBAL_STACK_SIZE (next_opus_pseudostack_limit)

#define ENABLE_HARDENING 1
#define OVERRIDE_celt_fatal 1

#define OVERRIDE_OPUS_ALLOC 1
#define OVERRIDE_OPUS_REALLOC 1
#define OVERRIDE_OPUS_FREE 1
#define OVERRIDE_OPUS_ALLOC_SCRATCH 1
void *opus_alloc(size_t size);
void *opus_realloc(void *ptr, size_t size);
void opus_free(void *ptr);
void *opus_alloc_scratch(size_t size);

#if defined(__aarch64__) || defined(__arm64__)
/* Cortex-A57 / Apple Silicon: NEON is always present, no run time CPU detection */
#define OPUS_ARM_MAY_HAVE_NEON_INTR 1
#define OPUS_ARM_PRESUME_NEON_INTR 1
#define OPUS_ARM_PRESUME_AARCH64_NEON_INTR 1
#endif

#endif
