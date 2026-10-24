/* The fixture ROM: all of it in the cartridge (it has no initialised data). */
ENTRY(_start)
SECTIONS
{
    . = 0x08000000;
    .text : { KEEP(*(.crt0)) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) *(.bss .bss.* COMMON) }
    ASSERT(SIZEOF(.data) == 0, "the fixture ROM can't have data or bss")
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
