# XNF kernel overview

XNF is a small educational 32-bit x86 kernel. It boots through a legacy BIOS,
enters protected mode, establishes paging and memory allocators, then runs
two timer-scheduled demonstration threads. It is
freestanding: there is no host OS, C runtime startup, or standard library to
initialize globals, allocate memory, or print messages on its behalf.

## Reading order

| Guide | Subject |
| --- | --- |
| [BOOT.md](BOOT.md) | From BIOS entry to C: CPU requirements, addresses, descriptors, stacks |
| [MEMORY.md](MEMORY.md) | Firmware memory maps, physical ownership, paging, virtual allocation |
| [HEAP.md](HEAP.md) | Aligned byte allocations, calloc, coalescing, and page reclamation |
| [HELPERS.md](HELPERS.md) | Managed strings and borrowed-pointer containers |
| [OBJECTS.md](OBJECTS.md) | Typed objects, named registry, and reference ownership |
| [IO.md](IO.md) | VGA cells, hardware cursor, port I/O, formatting API |
| [INTERRUPTS.md](INTERRUPTS.md) | Exceptions, normalized stack frames, PIC, timer, idle loop |
| [THREADS.md](THREADS.md) | Experimental kernel threads and context switches |
| [BUILD.md](BUILD.md) | Freestanding toolchain, disk-image pipeline, optional tests and validation |

[AGENTS.md](../AGENTS.md) holds repository editing and verification instructions.
The technical guides explain current behavior and limitations for any reader.

## Initialization order

1. The BIOS executes [asm/boot.asm](../asm/boot.asm) at physical `0x7C00`.
   The sector loads the kernel from the next disk sector to `0x10000` and
   far-jumps to `1000:0000`.
2. [asm/kernel.asm](../asm/kernel.asm) uses a temporary stack in the loaded image,
   checks the i686 CPU baseline, collects the BIOS E820 map, enables A20,
   installs a flat GDT, and enters 32-bit protected mode with IRQs disabled.
3. [src/boot_memory.c](../src/boot_memory.c) validates the entire kernel extent
   before assembly uses the final stack or clears `.bss`.
4. Assembly installs the exception/timer infrastructure and calls
   [kernel_main](../src/kernel.c).
5. C initializes the console and normalized memblock map, prepares and enables
   paging, initializes PMM, and permanently retires bootstrap allocation.
   A test build additionally runs the PMM ownership self-test.
6. VMM initializes its virtual arena, then the heap and object manager initialize
   without reserving backing pages. Normal boot enables interrupts, allocates
   two thread stacks, starts the scheduler, and retires the bootstrap context.
   Two threads print A and B approximately once per second; idle uses `STI; HLT`
   while both sleep. Test builds retain the bootstrap idle loop for fixtures.

The order is part of correctness: early C cannot use zero-initialized globals
before `.bss` is cleared, PMM needs the normalized reservations, and VMM needs
both paging and physical allocation. Boot enables maskable interrupts only
when the infrastructure and allocators are ready.

## Subsystem boundaries

| Sources | Responsibility |
| --- | --- |
| `asm/boot.asm`, `asm/kernel.asm` | BIOS loading and processor startup |
| `src/boot_memory.c`, `src/memory.c` | Early RAM validation and byte-granularity firmware map |
| `src/pmm.c` | Physical frame ownership, independent of virtual addresses |
| `src/paging.c`, `asm/paging.asm` | Page-table mechanics and hardware activation |
| `src/vmm.c` | Virtual range ownership and mappings backed by PMM frames |
| `src/kheap.c` | Byte-sized object allocations inside VMM-backed extents |
| `src/helpers.c` | Managed strings, byte helpers, and borrowed-pointer containers |
| `src/object.c` | Typed object lifetime and named-object registry |
| `src/conio.c` | Text output, formatting, and cursor/color state |
| `asm/interrupts.asm`, `src/interrupts.c` | Hardware entry/exit and exception diagnostics |
| `asm/threads.asm`, `src/scheduler.c` | Experimental kernel thread entry and scheduling |
| `src/kernel.c` | Initialization order, timer bookkeeping, A/B demonstration |
| `src/panic.c` | Fatal errors after the normal console becomes available |

Public C interfaces live in `inc/`. `paging_internal.h` is deliberately an
implementation interface: installing a translation does not establish
ownership of either the virtual slot or the physical frame.

## Current scope

The kernel runs in ring 0 on one CPU with a simple preemptive scheduler; core
shared services exclude preemption internally on that CPU. Virtual
addresses below `0x10000000` belong to the kernel; higher addresses are
reserved for future user processes and remain unmapped. This boundary says
nothing about where the kernel's physical RAM must come from.

VMM can return zeroed, virtually contiguous allocations in units of 4 KiB.
The heap provides 16-byte-aligned `kmalloc`/`kcalloc` objects and `kfree`, returning
empty extents to VMM. There is no user process, syscall layer, filesystem, keyboard
driver, or disk driver beyond the BIOS loader. The PIT generates timer IRQs
at approximately 100 Hz. Kernel section protection, guard pages, demand paging,
and SMP synchronization are future work, not implied by the existing paging.
