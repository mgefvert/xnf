SECTIONS
{
    /* The BIOS loads the first kernel byte at physical 0x10000. */
    . = 0x10000;
    __kernel_start = .;

    .text :
    {
        *(.text)
        *(.text.*)
        *(.text$*)
    }

    .rodata :
    {
        *(.rodata)
        *(.rodata.*)
        *(.rdata)
        *(.rdata.*)
        *(.rdata$*)
    }

    .data :
    {
        *(.data)
        *(.data.*)
        *(.data$*)
    }

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(.bss$*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    }

    /* Reserve a page-aligned 16 KiB stack after .bss; the x86 stack grows down. */
    . = ALIGN(4096);
    .stack (NOLOAD) :
    {
        __kernel_stack_bottom = .;
        . += 16K;
        __kernel_stack_top = .;
    }

    /* Exclusive end includes the entire reserved bootstrap stack. */
    __kernel_end = .;
    ASSERT(__kernel_end <= 0xA0000, "kernel extent overlaps VGA/firmware aperture")

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.drectve)
    }
}
