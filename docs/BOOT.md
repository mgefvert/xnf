# XNF boot and processor setup

This guide follows [asm/boot.asm](../asm/boot.asm),
[asm/kernel.asm](../asm/kernel.asm), [src/boot_memory.c](../src/boot_memory.c), and
[kernel.ld](../kernel.ld). See the [overview](OVERVIEW.md) for the whole
kernel and [build guide](BUILD.md) for how the flat image is produced.

## BIOS loader

The first disk sector is exactly 512 bytes. The BIOS places it at physical
`0x7C00`, with the boot drive number in `DL`. The loader initializes its
segments and grows a small stack downward from `0x7C00`, away from its code.

INT `13h`, function `41h`, checks extended disk support. Function `42h` then
uses a Disk Address Packet (DAP) to read the complete kernel starting at LBA 1
into `1000:0000`, physical `0x10000`. The generated sector count is embedded
in that packet. One request is limited to 127 sectors, so the build rejects
larger images rather than letting the loader read only part of the kernel.
Disk failure writes a red `E` directly to VGA memory and halts.

The successful far jump sets both `CS` and `IP`. A BIOS boot does not provide
a C calling convention, global initialization, or protected-mode descriptors;
the kernel must establish those itself.

## Real-mode addresses and CPU baseline

The kernel initially uses `CS=DS=SS=0x1000`. A real-mode physical address is
`segment * 16 + offset`, so physical `0x10000` is offset zero in that segment.
Symbols are linked at physical addresses, but BIOS-facing data operands need
segment-relative offsets. `RM_OFFSET(label)` supplies those offsets;
`KERNEL_ADDRESS(label)` supplies physical addresses needed for the transition.

The initial 2 KiB temporary stack lives in the loaded `.data` section. It
does not rely on the unvalidated final stack or uninitialized `.bss`.

XNF requires i686 (Pentium Pro or later), matching `-march=i686`. Assembly
tests whether EFLAGS.ID can be changed before executing CPUID, then rejects
base CPU families below 6. This conservative baseline supplies CR4 and
supervisor write protection needed by paging. The E820 routine's local use
of 386-era instructions does not make a 386 sufficient for the whole kernel.

## BIOS memory discovery

INT `15h`, function E820, returns physical address ranges. The caller supplies
the `SMAP` signature and an opaque continuation token in `EBX`; zero begins
the query and a returned zero ends it. Assembly collects at most 32 records
and marks incomplete/error results so later initialization cannot silently
use a truncated map.

The stored layout is 24 bytes per record: 64-bit base, 64-bit length, 32-bit
type, and 32-bit attributes. Legacy 20-byte responses are normalized to
attributes `1`, meaning enabled. An extended record whose attribute bit 0
is clear stays disabled. The shared C layout is in [inc/e820.h](../inc/e820.h).
Record order is retained; normalization into non-overlapping regions happens
later in [memory management](MEMORY.md).

## Protected-mode transition

The fast A20 gate at port `0x92` enables address line 20 while leaving its
reset bit clear. The GDT contains a null descriptor, a flat code descriptor,
and a flat data descriptor. Selector `0x08` identifies code; `0x10` identifies
data and stack. Their bases are zero and their limits span 4 GiB.

Assembly loads GDTR, sets CR0.PE, and uses a 32-bit far jump to load the code
selector. It then loads the data selectors. From here, linked symbols already
have their correct physical/linear addresses: adding `0x10000` again would
address the wrong memory. Paging is still disabled at this point.

## Validate before clearing or using RAM

On the temporary loaded stack, `boot_memory_check()` reads the raw collected
map. It uses no `.bss` state, normal console calls, or allocator services.
It rejects incomplete discovery, enabled address/length overflow, enabled
non-available overlaps with the kernel, and holes in usable coverage.

Coverage may come from several adjacent or overlapping usable records. The
check repeatedly extends the proven prefix without requiring BIOS order.
Reserved overlaps have already been rejected, so a usable record cannot
conceal a firmware reservation. Disabled records are ignored entirely.

Failure prints a fixed message through raw VGA stores and halts with IRQs
disabled. It does not clear `.bss` or use the final stack. After success,
assembly switches to the linker-reserved 16 KiB stack and zeroes exactly
`[__bss_start, __bss_end)`. Only then are C globals' zero-initialization
assumptions valid. The final stack itself is reserved but need not be zeroed.

## Linker layout and C interface

`__kernel_start` is `0x10000`. Loaded `.text`, `.rodata`, and `.data` come first.
The assembly entry object is linked first so its entry is the first byte.
`.bss` and `.stack` are `NOLOAD`: they occupy RAM but contribute no bytes to
the flat file. `__kernel_end` is exclusive and includes alignment padding and
the final stack. The linker caps it at `0xA0000`, while E820 validation catches
firmware reservations below that coarse VGA-aperture boundary.

C functions use 32-bit cdecl: arguments right to left, caller removes them;
EAX/ECX/EDX are caller-saved and EBX/ESI/EDI/EBP are callee-saved. Assembly
aligns C call sites to 16 bytes where needed and clears the direction flag.
MinGW decorates C symbols with a leading underscore; declarations of special
linker symbols use GCC `__asm__` names to bypass that decoration.

Assembly initializes interrupts and calls `kernel_main()`. The subsequent
allocator order is described in [OVERVIEW.md](OVERVIEW.md).
