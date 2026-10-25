# Working on XNF

XNF is a tiny educational i686 kernel in NASM and freestanding C. Start with
[KERNEL_OVERVIEW.md](KERNEL_OVERVIEW.md) for the architecture and reading order.
This file describes how to work on the repository; subsystem contracts live
in the technical guides.

## Documentation map

| Guide | Read when changing |
| --- | --- |
| [KERNEL_OVERVIEW.md](KERNEL_OVERVIEW.md) | Initialization order, scope, or subsystem boundaries |
| [KERNEL_BOOT.md](KERNEL_BOOT.md) | BIOS entry, CPU checks, addressing, GDT, stacks, or linker layout |
| [KERNEL_MEMORY.md](KERNEL_MEMORY.md) | E820, memblock, PMM, paging, VMM, or synchronization |
| [KERNEL_HEAP.md](KERNEL_HEAP.md) | Byte allocations, splitting/coalescing, calloc, or extent reclamation |
| [KERNEL_IO.md](KERNEL_IO.md) | VGA output, cursor/color handling, formatting, or port I/O |
| [KERNEL_INTERRUPTS.md](KERNEL_INTERRUPTS.md) | Exception frames, IDT/PIC, timers, faults, or idle behavior |
| [KERNEL_BUILD.md](KERNEL_BUILD.md) | Compiler flags, generated artifacts, build dependencies, or QEMU tests |

## Editing rules

- Read the relevant guide and source before editing. Preserve documented
  ordering, ownership, ABI, and synchronization contracts; update the guide
  when a contract changes.
- Keep a purpose header in every source, header, assembly, build, and test
  file. Explain why operations exist and decode non-obvious masks, shifts,
  alignment, and lifetime rules. Avoid comments that merely repeat syntax.
- Keep sources and build scripts under version control. Generated objects,
  images, sector includes, and test artifacts belong in the build directory.
- Track included assembly files in the NASM custom command's dependencies.
  Keep CLion's C code model and the actual freestanding compile flags aligned.
- Preserve the pre-.bss RAM check and allocator handover rules. Never treat a
  physical allocation as a pointer or bypass VMM ownership through raw paging.
- Keep hardware entry/exit in assembly and policy/bookkeeping in C. Changes
  to assembly exception push order must update the C frame layout together.
- Heap and VMM allocation require one CPU and non-preemptive normal context.
  Do not introduce process address spaces or new allocation contexts unless requested.

## Validation workflow

Use the existing normal configuration (`cmake-build-debug`) and a separate
`cmake-build-tests` configured with `-DXNF_BUILD_TESTS=ON`. See
[KERNEL_BUILD.md](KERNEL_BUILD.md) for setup and tool discovery.

```text
cmake --build cmake-build-debug
cmake --build cmake-build-tests
python tests/boot_qemu.py --build cmake-build-debug
python tests/boot_qemu.py --build cmake-build-tests
python tests/pmm_qemu.py --build cmake-build-tests
python tests/vmm_qemu.py --build cmake-build-tests
python tests/kheap_qemu.py --build cmake-build-tests
```

Run PMM/VMM/heap suites for memory changes, boot checks for startup/linker changes,
and the affected QEMU checks for console/interrupt changes. Check successful
initialization, control registers, continued timer progress, and fault/reset
logs. Discover addresses from symbols and CR3; never hard-code a page-directory
address. Expected faults belong in disposable guests. Keep VMM write fixtures
within actual emulated RAM; PMM ownership-only fixtures may describe other RAM.

Report what changed and which checks ran. Keep durable technical explanations
in the guides rather than expanding this agent checklist.
