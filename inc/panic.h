#ifndef XNF_PANIC_H
#define XNF_PANIC_H

__attribute__((noreturn)) void kernel_panic(const char *subsystem, const char *reason);

#endif
