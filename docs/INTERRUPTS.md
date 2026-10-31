# XNF interrupts and diagnostics

[asm/interrupts.asm](../asm/interrupts.asm) handles processor entry/exit, IDT/PIC/PIT
setup, the timer IRQ, and a software yield gate. [src/interrupts.c](../src/interrupts.c)
interprets fatal exception frames; [src/kernel.c](../src/kernel.c) owns timer bookkeeping and idle
policy. See [boot](BOOT.md) for the flat ring-0 segment assumptions.

## IDT and PIC

The IDT has 256 descriptors in reserved kernel `.bss`. Initialization clears
all descriptors, installs the 32 CPU exception gates, IRQ0, and a ring-0
software yield gate at `0x30`, then loads IDTR. A gate contains the handler's address split into low/high 16-bit
pieces and the flat code selector `0x08`. Gates are present, ring-0, 32-bit
interrupt gates (`0x8E`); hardware clears IF when entering one.

The legacy master/slave 8259 PICs initially occupy vector ranges that conflict
with CPU exceptions. Initialization remaps master IRQs to `0x20..0x27` and
slave IRQs to `0x28..0x2F`. It programs the IRQ2 cascade and 8086 mode, then
masks all lines except master IRQ0. IRQ2 remains masked, so slave IRQs cannot
arrive yet. Port `0x80` writes provide legacy delay between PIC commands.

| Ports | Device interface |
| --- | --- |
| `0x20`, `0x21` | Master PIC command/data |
| `0xA0`, `0xA1` | Slave PIC command/data |

## Uniform exception frames

Some CPU exceptions push an error code and others do not. The stubs push a
synthetic zero where needed, then push the vector number so C always sees
one layout. `PUSHAD` saves general registers before the common stub passes
the frame pointer to C. Stack alignment padding is below that saved frame;
EBP remembers the original frame address so it can be restored.

The current `ExceptionFrame` matches these 32-bit words, beginning at the
lowest address handed to C:

| Byte offset | Field |
| --- | --- |
| 0, 4, 8, 12 | EDI, ESI, EBP, PUSHAD's saved ESP |
| 16, 20, 24, 28 | EBX, EDX, ECX, EAX |
| 32, 36 | Vector, normalized error code |
| 40, 44, 48 | CPU-saved EIP, CS, EFLAGS |

Saved ESP is the value PUSHAD recorded; it is not a separate user stack.
This is a same-privilege ring-0 frame. Future user transitions would require
accounting for the CPU's additional stack-switch frame and segment handling.
Any change to assembly push order must change the C structure together.
An interrupt interface header and compile-time offset checks remain useful
follow-ups as the interface grows.

The handler currently never returns. The common assembly epilogue nevertheless
restores registers, discards vector/error words, and uses `IRETD` to restore
the CPU frame if recovery is implemented later.

## Page faults and terminal errors

Vector 14 is a page fault. The C handler captures CR2 before console output
could cause another fault and overwrite the original faulting linear address.
It prints the vector, error code, instruction address, saved flags, and page
fault interpretation:

| Error bit | Meaning when set |
| --- | --- |
| 0 | Protection violation rather than a non-present mapping |
| 1 | Write rather than read access |
| 2 | User rather than supervisor access |
| 3 | Reserved paging bit violation |
| 4 | Instruction-fetch indication where the CPU/features provide it |

Unrelated exception codes are not decoded using page-fault meanings. The
kernel has no fault recovery, demand paging, or task to switch to; fatal
diagnostics end in `CLI; HLT`. Deliberate fault tests therefore use separate
disposable guests and verify the address, error bits, halt state, and logs.

## Timer and idle loop

The PIT generates the timer signal. During interrupt initialization, with CPU
interrupts disabled, XNF programs PIT channel 0 via command port `0x43` and
data port `0x40`. Command `0x34` selects a binary mode 2 rate generator with
low-byte-then-high-byte divisor writes. Divisor 11,932 rounds the approximately
1,193,182 Hz input to approximately 100 Hz. The PIC routes the pulses to IRQ0.

IRQ0 preserves the general registers, clears DF, aligns the C call sites, and
increments `timer_ticks`. It then asks the scheduler to select a saved frame;
when the experimental scheduler is inactive this is the interrupted frame.
It acknowledges the master PIC with end-of-interrupt command `0x20` before
restoring the selected frame and returning with `IRETD`. The software yield
gate uses the same frame without acknowledging the PIC. Console output and
allocation stay out of these handlers. See [threads](THREADS.md) for
the frame, startup, and execution restrictions.

The ordinary loop reads the volatile counter and prints an occasional dot.
Unsigned subtraction tolerates counter wrap. The loop displays a dot every
100 ticks, approximately one second; the integer divisor is not exact.

`interrupts_enable_and_halt()` executes `STI; HLT`. STI delays recognition
through the following instruction, so a pending timer cannot arrive between
enabling IRQs and sleeping, leaving the kernel asleep until another IRQ.
The interrupt return restores the saved IF; this mechanism is distinct from
the short saved-EFLAGS guards used by [memory management](MEMORY.md).
