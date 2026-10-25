/*
 * Shared page geometry and physical/virtual address-space limits.
 * The kernel virtual boundary is an address-space policy, not a physical RAM split.
 */

#ifndef XNF_MM_H
#define XNF_MM_H

#define PAGE_SIZE 4096u
/* Exclusive limits. 4 GiB needs 64 bits even though every valid frame base
   fits in 32 bits; 256 MiB divides virtual kernel/user space only. */
#define KERNEL_VIRTUAL_END 0x10000000u
#define PHYSICAL_END 0x100000000ULL

#endif
