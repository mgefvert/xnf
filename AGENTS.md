# XNF project notes

XNF is a small educational x86 kernel written in NASM and C. `asm/boot.asm` is a 16-bit
BIOS boot sector. It loads the flat kernel image at physical address `0x10000`
and jumps to it as `1000:0000`. `asm/kernel.asm` begins in 16-bit real mode, gathers
the BIOS E820 memory map, enables A20, installs a flat GDT, and enters 32-bit
protected mode. The C initialization path prepares and enables 32-bit identity
paging before entering the timer-driven idle loop. There is no C runtime.

## Source and build files

- Keep NASM and C sources, headers, and CMake scripts under version control.
- CMake-generated binaries, the padded kernel, sector include, and disk image
  belong in the build directory.
- `asm/kernel.asm` is assembled with NASM as a 32-bit COFF object; its first
  section still begins the image at `0x10000`. `asm/paging.asm` and
  `asm/interrupts.asm` are included after switching to `BITS 32`; changes to
  included assembly files must be tracked by the CMake command's dependencies.
- Sources under `src/` are compiled as freestanding 32-bit C with CLion's
  MinGW GCC and `-march=i686`. Real-mode entry checks CPUID and rejects CPU
  families older than Pentium Pro before C runs. The build links only kernel
  objects, uses `kernel.ld` for section
  placement, and converts the linker output to a flat image for the existing
  BIOS pipeline.
- BIOS E820 collection remains in `asm/kernel.asm`; `src/memory.c` imports and
  displays the collected entries when called from `src/kernel.c`.
- Entry uses a 2 KiB temporary stack inside the loaded image. Before touching
  `.bss` or the final stack, `boot_memory_check()` validates the entire kernel
  extent against E820 without using `.bss`, allocators, or normal console code.
  Failure writes directly to VGA and halts. Only then may entry switch to the
  final stack and clear `.bss`. Preserve this order; there is no C runtime.
- Normal builds exclude C tests and deliberate fault probes. A separate build
  configured with `-DXNF_BUILD_TESTS=ON` includes `tests/kernel/*.c` and runs
  the PMM boot self-test. Test declarations belong in `inc/kernel_test.h`.
- The boot sector loads the kernel in one BIOS extended-read request, limited
  to 127 sectors.

## Assembly conventions

- Real-mode code uses `CS=DS=SS=0x1000`. In `asm/kernel.asm`, use
  `RM_OFFSET(label)` for segment-relative references and `KERNEL_ADDRESS(label)`
  for physical addresses needed across the protected-mode transition.
- Protected-mode segments are flat with base zero (`CS=0x08`, data/stack
  selectors `0x10`). Linked symbols already carry their physical/linear
  addresses; do not add the kernel load address to them again.
- Public console functions use 32-bit cdecl: arguments are pushed right to
  left, the caller removes arguments, `EAX`, `ECX`, and `EDX` are caller-saved,
  and `EBX`, `ESI`, `EDI`, and `EBP` are callee-saved.
- Console output uses 80x25 VGA color text memory at `0xB8000`. Character
  attributes and the software cursor are maintained in `src/conio.c`; the cursor
  position is mirrored to the VGA CRTC at ports `0x3D4`/`0x3D5`.
- `ksetcursor(column, row)` clamps coordinates to the screen. `kgetcursor`
  takes writable `uint32_t *column` and `uint32_t *row` output pointers.
- `kprint_hex` prints eight uppercase hexadecimal digits with a `0x` prefix;
  `kprint_hex64` prints a 64-bit value the same way with sixteen digits;
  `kprint_dec` prints an unsigned 32-bit decimal value.
- `kprintf` supports `%s`, `%c`, signed `%d`, `%x`, `%X`, and `%%`. `%X` is an
  XNF extension that consumes a 64-bit value; `%x` uses `kprint_hex` formatting.
- Console functions are implemented in C and use the same 32-bit cdecl
  convention when called from assembly.

Keep assembly straightforward and comment real-mode/protected-mode assumptions
where they affect addresses or BIOS calls.

## Memory and paging

- `src/memory.c` normalizes E820 ranges into ordered, merged regions, with
  conservative precedence for reserved memory. Preserve its overflow checks,
  region validation, and reservation of the complete linker-defined kernel
  extent, including `.bss` and the 16 KiB stack.
- Assembly normalizes legacy 20-byte E820 attributes to 1 (enabled). Both
  early validation and memblock import ignore extended entries with bit 0
  clear, including their lengths. Keep `inc/e820.h` synchronized with assembly.
- `memblock_alloc` is a bootstrap physical allocator; ordinary boot currently
  makes no allocations. Any future uses must run before paging. `pmm_init()`
  permanently retires new bootstrap
  allocations after importing reservations; later attempts panic with a clear
  diagnostic. Existing allocations and the region map remain valid.
- `paging_prepare()` builds supervisor-only, writable, non-PAE 4 KiB identity
  mappings. Page zero remains unmapped. The mapped extent includes kernel code,
  data, stack, GDT, IDT, interrupt handlers, VGA text memory, and all bootstrap
  allocations, including the page directory and tables themselves.
- The first 256 MiB of virtual addresses belong to the kernel; higher
  addresses remain unmapped for future user processes. This is a virtual
  reservation, not a reservation of 256 MiB of physical RAM.
- All 64 kernel page tables (256 KiB) and the directory (4 KiB) are preallocated
  in page-aligned `.bss` and covered by the permanent identity map. Preparation
  allocates nothing; its required extent includes all linker reservations and
  any earlier bootstrap allocations. `paging_identity_end` marks the
  permanent prefix and start of the VMM arena. Reject boot extents that leave
  no arena or would map the future user range.
- `paging_enable()` follows `paging_prepare()` immediately. The cdecl assembly
  routine `paging_activate(directory_physical)` saves EFLAGS, disables maskable
  interrupts, clears CR4.PAE/PSE, loads CR3, and sets CR0.PG/WP while preserving
  other CR0 bits. WP enforces supervisor read-only mappings. It restores
  EFLAGS after the transition and returns to C.
  Activation allocates no memory and writes no page-table entries.
- C checks CR0.PG/WP, CR3, and CR4.PAE/PSE before reporting success. Page-table
  verification currently runs before activation; hardware can subsequently
  set accessed/dirty bits, so exact entry comparisons need care afterward.
- The writable permanent identity prefix is a bootstrap design. Read-only
  kernel sections and stack guard pages remain future work. Runtime map,
  unmap, and protect are internal VMM operations; `paging_query` is read-only.

## Physical memory manager (stage 1)

- `pmm_init()` runs after paging activation and before the timer idle loop.
  `src/pmm.c` uses one statically allocated 128 KiB bitmap in `.bss` to track
  all 1,048,576 physical frames below 4 GiB. The linker reservation includes
  the bitmap. Only full pages of normalized `MEMBLOCK_AVAILABLE` regions are
  managed; page zero and all kernel/bootstrap reservations stay unavailable.
- `pmm_alloc_page()` returns a physical address using one circular bitmap
  search and one word hint. There is no low/high split or promised allocation
  order. Zero indicates failure; allocation neither maps nor clears a frame.
- The kernel virtual range is the lowest 256 MiB, but the permanent identity
  prefix maps only its required extent (normally 4 MiB). The remaining kernel
  virtual addresses are available to VMM allocations with arbitrary backing.
  A low physical address does not itself prove a frame is currently mapped.
  Check existing mappings before dereferencing; never directly dereference an
  ordinary physical allocation without a suitable mapping.
- A set bitmap bit represents either a reservation or a PMM allocation.
  `pmm_free_page()` consults memblock's map to distinguish them and rejects
  page zero, unaligned addresses, permanent reservations, and double frees.
  Managed/free counters exclude permanent reservations. Bitmap/counter changes
  preserve IF and disable IRQs on the current single-CPU kernel.
- After importing the free pool and reservations, `pmm_init()` calls
  `memblock_retire()` with IRQs still disabled, then publishes PMM ownership
  and reports the handover. The PMM is the sole physical page allocator from
  that point. Retirement only blocks new bootstrap allocations; it preserves
  all reservation metadata used by PMM free checks and has no reverse operation.
- The test-build-only `pmm_self_test()` checks distinct/aligned physical loans,
  counters, and invalid frees. It never maps or dereferences physical frames.
- Run `python tests/pmm_qemu.py --build cmake-build-tests` after building with
  `-DXNF_BUILD_TESTS=ON`.
  This checks every bitmap bit, 128/512 MiB boots, timer progress, and fault
  logs. With GDB available (or `--gdb PATH`), synthetic E820 tests also cover
  overlapping reservations in both orders, partial frames, disjoint regions,
  and the last frame below 4 GiB. A separate disposable guest attempts a late
  bootstrap allocation and verifies the retirement panic, halt, and unchanged
  PMM state. Test artifacts stay in the build directory.

## Kernel virtual page allocator

- `vmm_init()` runs after PMM initialization/self-test, before the timer loop.
  The public interface in `inc/vmm.h` provides zeroed, writable, supervisor-only
  contiguous virtual page allocations and whole-allocation freeing; no heap
  exists yet. Physical frames may be noncontiguous and above 256 MiB.
- Three fixed bitmaps (24 KiB total in `.bss`) track occupancy and exact
  allocation boundaries. Permanent-prefix slots are never allocated/freed.
  Invalid sizes/exhaustion return null; invalid/interior/double frees return
  false. Corrupt internal ownership/mappings panic. Preserve complete rollback
  after partial physical allocation and unmap/invalidate before PMM release.
- `paging_internal.h` is only for VMM implementation/tests. Map/unmap/protect
  require virtual-slot ownership in normal context, never change physical
  ownership, and cannot change the permanent prefix or future user range.
  Do not bypass VMM bookkeeping or free backing frames directly through PMM.
- VMM operations preserve IF but do not mask IRQs during scans or zeroing.
  Individual PMM operations and PTE edits/invalidation mask IRQs briefly.
  Transactions rely on one CPU, no preemptive scheduler, and no IRQ/exception
  allocation. Add synchronization before introducing preemption or SMP.
- Run `python tests/vmm_qemu.py --build cmake-build-tests` as well as the PMM
  suite. It requires GDB (optional `--gdb PATH`), checks real 128/512 MiB guests,
  complete PMM/VMM bitmap restoration, page tables, timers, exhaustion/rollback,
  fragmentation, permissions, IF, and four disposable expected-fault guests.
  A controlled fixture with high frames and a reserved hole guarantees high,
  noncontiguous backing independently of PMM allocation order. The ordinary
  512 MiB guest verifies IRQ delivery during a large allocation.
  Explicit VMM test entry points are not run during ordinary boot: synthetic
  PMM E820 fixtures can describe high physical RAM not actually backed by QEMU.
- The linker caps the kernel below the VGA aperture; preserve memblock's
  stronger E820 check and the pre-.bss check of the entire kernel reservation.
  See `KERNEL_MEMORY.md` for contracts and the virtual layout.
- Run `python tests/boot_qemu.py --build cmake-build-debug` and against the
  test build. It validates CPU rejection, legacy attributes, normal boots,
  and early rejection with sentinel bytes proving `.bss` and the final stack
  were not touched. Shared `tests/qemu_test.py` has no import-time execution.
- Shared memory constants live in `inc/mm.h`. Normal post-.bss fatal errors
  use `kernel_panic(subsystem, reason)`, which disables IRQs before console
  output. Early boot must use its separate raw VGA diagnostic.

## Interrupts and diagnostics

- Assembly normalizes exception frames before calling C. Keep the C
  `ExceptionFrame` layout synchronized with the push order in
  `asm/interrupts.asm`. Page-fault diagnostics capture CR2 before console output.
- IRQ0 preserves general registers, calls a short C handler that increments
  `timer_ticks`, and acknowledges the master PIC. Console output stays in the
  normal kernel loop; the console currently has no synchronization.
- The idle path uses `STI; HLT`. The PIT is not programmed by XNF yet, so its
  rate depends on firmware setup; 18 ticks is only approximately one second
  under the usual BIOS configuration.
- For paging changes, build with the existing CMake configuration and validate
  in QEMU: check the success diagnostic and control registers, continued timer
  output, and absence of faults or unexpected resets. Do not assume a fixed
  page-directory address; it depends on earlier allocations.

## Review follow-ups

- Consider an interrupt interface header and compile-time exception-frame
  layout checks as that interface grows. Keep hardware entry/exit in assembly
  and bookkeeping and policy in C.
