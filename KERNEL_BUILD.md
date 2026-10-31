# Building and validating XNF

The build creates a raw BIOS disk image, not a host executable to run directly.
See the [overview](KERNEL_OVERVIEW.md) for architecture and [boot guide](KERNEL_BOOT.md)
for the loader and linker assumptions that the build must preserve.

## Tools and configurations

CMake uses NASM, CLion's MinGW GCC/binutils, and QEMU `qemu-system-i386`.
NASM emits a 32-bit COFF object for the kernel entry and a flat binary for the
boot sector. MinGW GCC defaults to a 64-bit target, so the actual C commands
explicitly request `-m32 -march=i686`. There are no linked C runtime objects
or standard libraries.

The normal configuration is `cmake-build-debug`. Build it with the configured
CLion toolchain or the same tools from a shell:

```text
cmake --build cmake-build-debug
```

CMake caches absolute source and build paths. After moving a checkout, create
fresh build directories and configure them for the new location; reusing a
copied cache can point commands at the old checkout.

The `kernel_c_model` object target is excluded from the real image build. It
lets CLion understand the C files and headers; custom compile commands build
the real objects. Both use the same flags. Among those flags:

| Flag | Reason |
| --- | --- |
| `-m32 -march=i686` | Match the assembly, ABI, and checked CPU minimum |
| `-mno-sse -mno-mmx` | Startup does not initialize SIMD execution state |
| `-ffreestanding -fno-builtin` | Compile for a kernel without assuming hosted library services |
| `-fno-pie` | Use the fixed addresses expected by the flat image |
| `-fno-stack-protector` | Avoid runtime guard symbols and failure handlers |
| `-fno-asynchronous-unwind-tables -fno-unwind-tables` | No stack-unwinding runtime exists |
| `-Wall -Wextra -Werror` | Treat compiler diagnostics as build failures |

The real link passes only the assembly object and selected kernel C objects.
Adding a file under `src/` or `inc/` regenerates CMake's source/header lists.
Optional C tests live under `tests/kernel/` and are included only when
`XNF_BUILD_TESTS=ON`. Test functions stay outside normal allocator headers.

## Image pipeline

1. Assemble `asm/kernel.asm` with NASM `-f win32`. It includes `paging.asm`
   and `interrupts.asm` after switching to `BITS 32`; both files must remain
   explicit custom-command dependencies.
2. Compile the freestanding C files and link with `kernel.ld`. The assembly
   entry object comes first so the first image byte is the real-mode entry.
3. MinGW's PE-oriented linker produces an intermediate `kernel_linked.exe`
   whose section addresses match physical placement. It is a symbol source
   for tests, not a Windows program to launch.
4. `objcopy --change-addresses -0x10000 -O binary` produces `kernel.bin`.
   This arranges flat-file offsets; it does not change already-linked symbol
   references inside the code. `.bss` and the final stack consume RAM, not
   bytes in this flat file.
5. `cmake/PrepareKernel.cmake` rounds the file size upward to 512-byte sectors,
   checks the 127-sector loader limit, and produces `kernel.padded.bin` and
   `kernel_sectors.inc`. NASM generates any required zero padding.
6. Assemble `asm/boot.asm` with the generated include, embedding the actual
   kernel sector count in its DAP.
7. `cmake/CreateDiskImage.cmake` checks sizes and concatenates the 512-byte
   boot sector and padded kernel into `xnf.img`: boot is LBA 0, kernel LBA 1.

Generated objects, temporary padding files, binaries, includes, and disk images
belong in the build directory. The source tree holds source and build scripts.

## Running

The `run` target launches QEMU against the freshly built image:

```text
cmake --build cmake-build-debug --target run
```

Equivalently, launch QEMU with `-drive format=raw,file=PATH_TO_BUILD/xnf.img`.
The image should report memory initialization and paging success, reach
`Kernel started.`, and continue printing timer dots. XNF assumes the BIOS VGA
text mode; see [I/O](KERNEL_IO.md) and
[interrupts](KERNEL_INTERRUPTS.md).

## Validation

Normal and test builds share kernel behavior, except the test build includes
ownership self-tests and host-selectable test/fault entry points. Test scripts
discover addresses from linked symbols and CR3 rather than fixed physical
locations. The following commands and coverage describe the supported checks.

Normal builds exclude self-tests and deliberate fault probes. Configure a
separate test build with the normal build's compiler/generator, then run:

```text
cmake -S . -B cmake-build-tests -DXNF_BUILD_TESTS=ON
cmake --build cmake-build-tests
python tests/boot_qemu.py --build cmake-build-debug
python tests/boot_qemu.py --build cmake-build-tests
python tests/pmm_qemu.py --build cmake-build-tests
python tests/vmm_qemu.py --build cmake-build-tests
python tests/kheap_qemu.py --build cmake-build-tests
python tests/helpers_qemu.py --build cmake-build-tests
python tests/object_qemu.py --build cmake-build-tests
```

Shared infrastructure lives in `tests/qemu_test.py`, with no argument parsing
or process execution at import. C test sources live in `tests/kernel/` and
are included only with `XNF_BUILD_TESTS=ON`; their declarations live in
`inc/kernel_test.h`, outside the public allocator interfaces.

Helper tests use the heap harness in disposable guests to exercise strings,
containers, pressure rollback, and ownership cleanup. See
[KERNEL_HELPERS.md](KERNEL_HELPERS.md) for their contracts.

Object-manager tests use the same heap harness, plus isolated pre-heap and
invalid-release guests. See [KERNEL_OBJECTS.md](KERNEL_OBJECTS.md).

### Boot checks

The boot suite requires GDB. It checks ordinary 128/512 MiB boots, rejection
of 486/Pentium CPUs, the real legacy-response normalization branch, and
disabled reservations. Rejected reserved `.bss`/stack, gapped, disabled-RAM,
and overflowing layouts must leave sentinel bytes throughout `.bss` and the
final stack unchanged and remain halted without exceptions or resets.

### PMM checks

The PMM suite checks every physical bitmap bit against an independent E820
model, 128/512 MiB boots, timers, faults/resets, overlapping/disjoint/partial
regions, disabled entries, the top frame below 4 GiB, and bootstrap retirement.
The top-frame fixture has exactly three allocatable frames, compared as a
set; tests do not depend on allocation order. PMM tests never dereference
frames. Synthetic E820 and retirement cases require GDB; use `--gdb PATH` if discovery fails.

### VMM checks

The VMM suite requires GDB. It redirects disposable, normally booted guests
to explicit test entry points, so ordinary startup does not run exhaustive
tests. Real 128/512 MiB guests cover zeroing, data independence, page/table
boundaries, virtual reuse, noncontiguous physical frames, high physical RAM,
borrowed mappings, TLB remapping, permissions, invalid operations, physical
exhaustion with partial rollback, virtual exhaustion/fragmentation, and IF
preservation. A separate fixture exposes only high allocatable frames in real
512 MiB RAM, with a reserved hole guaranteeing noncontiguous backing. The
ordinary 512 MiB guest checks timer delivery during a large allocation.
Host checks compare the full PMM and VMM bitmaps before/after,
verify every kernel PTE and all future-user PDEs, and confirm timer progress.
Four further guests verify exact fault addresses/error codes and halted IRQ
state for null, freed, read-only-write, and user-boundary accesses. Logs and
snapshots stay in the build directory. VMM write fixtures describe only RAM
actually backed by the emulator; PMM-only fixtures may describe synthetic RAM.

### Heap checks

`tests/kheap_qemu.py` requires GDB and runs guest-side tests from
`tests/kernel/kheap_test.c`. Normal 128/512 MiB guests and a high-memory,
physically fragmented fixture cover alignment, non-overlap, boundary sizes,
data independence, block splitting/coalescing, dirty-block and multi-page
calloc, multiplication/alignment/page-rounding overflow, and mixed allocation
orders. A separate VMM reservation forces nonadjacent heap extents.

Physical/virtual exhaustion must preserve live payloads and leave failed
growth without ownership leaks. Existing free blocks must still work without
new pages, and minimal growth must use a final available page when four-page
growth fails. Releasing pressure must allow allocation to recover. Full PMM
and VMM bitmaps, page tables, counters, and the empty extent list are checked
after positive tests, with continued timer progress and preserved IF.

A preinitialization guest checks null allocation/no-op null free before heap
startup. Disposable negative guests verify aligned/unaligned interior frees,
double frees both inside live extents and after unmapping, foreign VMM and
unmapped addresses, and repeated/out-of-order initialization. These must panic
and remain halted with IRQs disabled and stable ownership. A final guest writes
through a released extent pointer and must fault at that address with the
expected non-present supervisor-write error code. See [KERNEL_HEAP.md](KERNEL_HEAP.md)
for the supported API and execution model.
