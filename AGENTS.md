# XNF project notes

XNF is a small educational x86 kernel written in NASM and C. `asm/boot.asm` is a 16-bit
BIOS boot sector. It loads the flat kernel image at physical address `0x10000`
and jumps to it as `1000:0000`. `asm/kernel.asm` begins in 16-bit real mode, gathers
the BIOS E820 memory map, enables A20, installs a flat GDT, and enters 32-bit
protected mode. There is no paging or C runtime.

## Source and build files

- Keep NASM and C sources, headers, and CMake scripts under version control.
- CMake-generated binaries, the padded kernel, sector include, and disk image
  belong in the build directory.
- `asm/kernel.asm` is assembled with NASM as a 32-bit COFF object; its first
  section still begins the image at `0x10000`. `asm/interrupts.asm` is included
  after switching to `BITS 32`.
- `src/kernel.c` and `src/conio.c` are compiled as freestanding 32-bit C with CLion's
  MinGW GCC. The build links only kernel objects, uses `kernel.ld` for section
  placement, and converts the linker output to a flat image for the existing
  BIOS pipeline.
- BIOS E820 collection remains in `asm/kernel.asm`; `src/kernel.c` displays the
  collected entries.
- The assembly entry clears linker-defined `.bss` bounds before calling
  `kernel_main`; there is no C runtime startup.
- The boot sector loads the kernel in one BIOS extended-read request, limited
  to 127 sectors.

## Assembly conventions

- Real-mode kernel labels are offsets in `CS=DS=SS=0x1000`; their physical
  addresses are `0x10000 + label`.
- Protected-mode data segments are flat with base zero. Use the physical/linear
  address `0x10000 + label` when a protected-mode reference needs a kernel
  symbol's address.
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
