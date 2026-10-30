# XNF

XNF is a small educational 32-bit x86 kernel written in NASM and freestanding C. It boots through a legacy BIOS, enters 
protected mode, initializes memory management, and idles while timer interrupts keep it running.

## Features

- BIOS bootloader and i686 protected-mode startup.
- E820 memory discovery, physical frame allocation, paging, and virtual memory management.
- A kernel heap with aligned allocations, zero-initialized allocations, and extent reclamation.
- VGA text output, exception diagnostics, and timer interrupts.
- Managed strings, containers, and a typed object registry.
- QEMU-based boot and subsystem tests.

XNF runs on a single CPU in kernel mode. It does not yet provide user processes, a filesystem, or keyboard and disk drivers.

## Build and run

The build uses CMake, NASM, MinGW GCC/binutils, and QEMU. With the CLion toolchain configured:

```sh
cmake --build cmake-build-debug
cmake --build cmake-build-debug --target run
```

The build produces a raw BIOS disk image, `cmake-build-debug/xnf.img`. See [KERNEL_BUILD.md](KERNEL_BUILD.md) for toolchain 
setup and test instructions.

## Documentation

Start with [KERNEL_OVERVIEW.md](KERNEL_OVERVIEW.md) for the architecture and links to the subsystem guides.

## Contributing

Contributions are welcome! See [CONTRIBUTING.md](CONTRIBUTING.md) for development, testing, and credit guidelines.

## Attribution

Most of the code was written by OpenAI's Codex.
