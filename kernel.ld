SECTIONS
{
    /* The BIOS loads the first kernel byte at physical 0x10000. */
    . = 0x10000;

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

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.drectve)
    }
}
