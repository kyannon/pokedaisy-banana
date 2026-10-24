@ The fixture ROM's start (in place of devkitARM's gba.specs): a branch over
@ the cartridge header, which mkheader.py fills in after linking, then the
@ stacks and a jump into main (Thumb). No logo: mGBA starts a ROM without the
@ BIOS's check, and this one never goes near real hardware.
    .section .crt0, "ax"
    .arm
    .global _start
_start:
    b       start
    .fill   0xBC, 1, 0          @ 0x04-0xBF: the header
start:
    mov     r0, #0x12           @ IRQ mode's stack
    msr     cpsr_c, r0
    ldr     sp, =0x03007FA0
    mov     r0, #0x1F           @ system mode's
    msr     cpsr_c, r0
    ldr     sp, =0x03007F00
    ldr     r0, =main
    mov     lr, pc
    bx      r0
1:  b       1b
    .ltorg
