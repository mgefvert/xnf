# XNF console and hardware I/O

[src/conio.c](../src/conio.c) and [inc/conio.h](../inc/conio.h) provide the console.
The BIOS loader reads the disk, but the running kernel's current I/O consists
of VGA text memory, the VGA cursor, and the interrupt-controller operations
described in [INTERRUPTS.md](INTERRUPTS.md). There is no keyboard
driver or general device framework yet.

## Text memory and color

The console assumes an 80-column, 25-row VGA color text screen at physical
`0xB8000`. Each cell is a 16-bit word: the low byte is the character and the
high byte is its attribute. A write forms `character | (attribute << 8)`.
The pointer is volatile because stores affect hardware even when C never
reads the stored value back.

Attribute bits 0..3 select foreground color and bits 4..6 select background.
Bit 7 depends on the existing VGA blink/background-intensity configuration;
XNF does not change that configuration. The default attribute is `0x07`.
`ksetcolor()` affects subsequent output; it does not recolor previous cells.
`clear_screen()` fills every cell with a space in the current attribute.

VGA memory remains identity-mapped in the permanent bootstrap prefix. It is
device memory, not PMM-managed RAM. Port I/O is a separate mechanism: `IN` and
`OUT` address hardware ports and do not need virtual mappings for those ports.

## Cursor and scrolling

The software cursor is a linear cell index, `row * 80 + column`. Division and
remainder recover coordinates. `ksetcursor(column, row)` clamps to the screen;
`kgetcursor(&column, &row)` requires two writable output pointers.

The hardware cursor is mirrored through the CRTC's index/data ports:

| Port/register | Purpose |
| --- | --- |
| `0x3D4` | Select a CRTC register |
| `0x3D5` | Read/write the selected register |
| Register `0x0A`, bit 5 | Cursor disable; cleared without changing cursor shape |
| Register `0x0F` | Low byte of cursor cell index |
| Register `0x0E` | High byte of cursor cell index |

Output wraps at the screen edge. When advancing beyond the last row, the
console copies rows upward, clears the final row, and leaves the cursor there.
Forward copying is safe because the destination lies below the source in
memory. Newline starts the next row; carriage return starts the current row;
backspace moves back and erases when a previous cell exists.

## Output API and formatting

All public functions use the kernel's [cdecl ABI](BOOT.md#linker-layout-and-c-interface).

| Function | Behavior |
| --- | --- |
| `kputc`, `kputs` | Output one character or a null-terminated string |
| `clear_screen` | Fill screen with spaces and reset cursor |
| `ksetcolor`, `kgetcolor` | Set/read the current attribute |
| `ksetcursor`, `kgetcursor` | Set/read cell coordinates |
| `kprint_hex` | `0x` plus eight uppercase hexadecimal digits |
| `kprint_hex64` | `0x` plus sixteen uppercase hexadecimal digits |
| `kprint_dec` | Unsigned 32-bit decimal |

The small `kprintf()` supports the following conversions; widths, precision,
length modifiers, and floating point are not implemented:

| Conversion | Argument and output |
| --- | --- |
| `%s` | Null-terminated string pointer |
| `%c` | Character promoted to `int` by C's variadic rules |
| `%d` | Signed `int` decimal |
| `%x` | Unsigned 32-bit value using `kprint_hex` formatting |
| `%X` | XNF extension: unsigned 64-bit value using `kprint_hex64` |
| `%%` | Literal percent sign, no argument |

`%X` differs from standard printf and consumes a different argument size than
`%x`; callers must pass the correct type. Hex printing extracts four-bit
nibbles, and the 64-bit printer prints two halves without runtime division
helpers. Decimal conversion collects remainders in reverse, then prints them
backward. Signed conversion computes magnitude with unsigned subtraction so
`INT_MIN` does not cause signed-negation overflow.

## Execution context and failures

The console has no synchronization. Timer IRQ code increments a counter and
does not print, keeping normal console state out of interrupt interleavings.
Fatal exception handlers still use the console for a final diagnostic and
halt; they do not implement concurrent or recoverable output.

`kernel_panic(subsystem, reason)` disables IRQs, prints through the initialized
console, and halts permanently. Pre-.bss boot failure must instead use the
raw VGA path because console cursor state is not initialized yet. QEMU tests
read VGA cells as physical memory, so these diagnostics are observable even
without a visible emulator window. See [validation](BUILD.md#validation).
