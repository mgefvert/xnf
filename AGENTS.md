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
  MinGW GCC. The build links only kernel objects, uses `kernel.ld` for section
  placement, and converts the linker output to a flat image for the existing
  BIOS pipeline.
- BIOS E820 collection remains in `asm/kernel.asm`; `src/memory.c` imports and
  displays the collected entries when called from `src/kernel.c`.
- The assembly entry clears linker-defined `.bss` bounds before calling
  `kernel_main`; there is no C runtime startup.
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
- `memblock_alloc` is a bootstrap physical allocator. Existing call sites run
  before enabling paging. `pmm_init()` permanently retires new bootstrap
  allocations after importing reservations; later attempts panic with a clear
  diagnostic. Existing allocations and the region map remain valid.
- `paging_prepare()` builds supervisor-only, writable, non-PAE 4 KiB identity
  mappings. Page zero remains unmapped. The mapped extent includes kernel code,
  data, stack, GDT, IDT, interrupt handlers, VGA text memory, and all bootstrap
  allocations, including the page directory and tables themselves.
- Preserve recalculation of the required mapping extent after each table
  allocation; a new table can extend the range that must be mapped.
- `paging_enable()` follows `paging_prepare()` immediately. The cdecl assembly
  routine `paging_activate(directory_physical)` saves EFLAGS, disables maskable
  interrupts, clears CR4.PAE/PSE, loads CR3, and sets CR0.PG while preserving
  other CR0 bits. It restores EFLAGS after the transition and returns to C.
  Activation allocates no memory and writes no page-table entries.
- C checks CR0.PG, CR3, and CR4.PAE/PSE before reporting success. Page-table
  verification currently runs before activation; hardware can subsequently
  set accessed/dirty bits, so exact entry comparisons need care afterward.
- The writable identity map is a bootstrap design. Read-only sections, stack
  guard pages, and a general mapping API are future work.

## Physical memory manager (stage 1)

- `pmm_init()` runs after paging activation and before the timer idle loop.
  `src/pmm.c` uses one statically allocated 128 KiB bitmap in `.bss` to track
  all 1,048,576 physical frames below 4 GiB. The linker reservation includes
  the bitmap. Only full pages of normalized `MEMBLOCK_AVAILABLE` regions are
  managed; page zero and all kernel/bootstrap reservations stay unavailable.
- `pmm_alloc_page()` returns a physical address, preferring frames at or above
  `0x10000000` to preserve low memory. `pmm_alloc_page_low()` restricts allocation
  to physical addresses below that boundary. Zero indicates failure. Neither
  function establishes a virtual mapping or clears the allocated frame.
- The intended kernel identity window is the lowest 256 MiB, but the existing
  paging setup currently maps only its required extent (normally 4 MiB).
  Being below the low limit does not itself prove a frame is currently mapped.
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
- `pmm_self_test()` checks low-page mappings before pattern writes, releases
  low/general allocations, checks counters and invalid frees, and prints its
  results. It installs no mappings and never dereferences general allocations.
- Run `python tests/pmm_qemu.py --build cmake-build-debug` after building.
  This checks every bitmap bit, 128/512 MiB boots, timer progress, and fault
  logs. With GDB available (or `--gdb PATH`), synthetic E820 tests also cover
  overlapping reservations in both orders, partial frames, disjoint regions,
  and the last frame below 4 GiB. A separate disposable guest attempts a late
  bootstrap allocation and verifies the retirement panic, halt, and unchanged
  PMM state. Test artifacts stay in the build directory.

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

- E820 extended attributes are collected but not honored during import. Handle
  disabled 24-byte entries while preserving valid legacy 20-byte entries,
  whose stored attributes are currently zero; simply rejecting every entry
  with attribute bit 0 clear would discard those legacy entries.
- Establish an explicit minimum CPU requirement: paging activation accesses
  CR4, so the E820 routine's local "80386 or later" assumption is insufficient
  as a requirement for the entire kernel.
- Consider an interrupt interface header and compile-time exception-frame
  layout checks as that interface grows. Keep hardware entry/exit in assembly
  and bookkeeping and policy in C.
