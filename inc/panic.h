/*
 * Declare the non-returning fatal-error path for initialized kernel services.
 * Early boot must use its own diagnostic because the normal console needs .bss.
 */

#ifndef XNF_PANIC_H
#define XNF_PANIC_H

__attribute__((noreturn)) void kernel_panic(const char *subsystem, const char *reason);

#endif
