/*
 * M0 bring-up stub: what does LK actually hand to the kernel on the E5?
 *
 * LK reads boot_b's kernel, copies it to 0x80080000 and jumps there with the
 * vendor FDT in x0 ("[start_linux] start linux at PC:0x80080000, dt_addr:0x86000000").
 * Measured result: the jump lands in EL2, not EL1. The first 64 bytes below are
 * a valid arm64 Image header so the blob also works if LK ever checks it.
 */
        .section .text.header, "ax"
        .globl  _image_start
_image_start:
        b       _entry                  /* code0: branch over the header */
        .word   0                       /* code1 */
        .quad   0                       /* text_offset */
        .quad   __image_size            /* image_size, resolved by m0.ld */
        .quad   0                       /* flags */
        .quad   0                       /* res2 */
        .quad   0                       /* res3 */
        .quad   0                       /* res4 */
        .word   0x644d5241              /* magic "ARM\x64" */
        .word   0                       /* res5 */

        .text
        .globl  _entry
_entry:
        msr     daifset, #0xf           /* no interrupts yet */
        adrp    x9, __stack_top
        add     x9, x9, :lo12:__stack_top
        mov     sp, x9
        /* .bss is NOLOAD: nothing zeroed it for us. */
        adrp    x10, __bss_start
        add     x10, x10, :lo12:__bss_start
        adrp    x11, __bss_end
        add     x11, x11, :lo12:__bss_end
2:      cmp     x10, x11
        b.hs    3f
        str     xzr, [x10], #8
        b       2b
3:
        bl      m0_main                 /* x0..x3 arrive untouched */
1:      wfe
        b       1b

/*
 * Drop from EL2 to EL1 and call x0 with SP_EL1 = x1 and VBAR_EL1 = x2.
 * This is the shim XNU will need: its start.s assumes it was entered at EL1,
 * but LK leaves us at EL2 with HCR_EL2.RW = 0 (so EL1 would be AArch32).
 */
        .globl  drop_to_el1
drop_to_el1:
        mov     x19, x0
        msr     sp_el1, x1
        msr     vbar_el1, x2
        msr     elr_el2, x19
        mov     x9, #0x3c5              /* EL1h, DAIF all masked */
        msr     spsr_el2, x9
        mov     x9, #1
        lsl     x9, x9, #31             /* HCR_EL2.RW = 1: EL1 is AArch64 */
        msr     hcr_el2, x9             /* also clears TGE/IMO/FMO/AMO */
        msr     cptr_el2, xzr           /* stop trapping FP/SIMD */
        isb
        eret

/*
 * PSCI CPU_ON entry point for the secondaries. We do not know in advance
 * whether firmware drops them at EL1 or EL2, so use SPSel to pick whichever SP
 * that level owns, and give each core its own stack.
 */
        .globl  sec_entry
sec_entry:
        msr     daifset, #0xf
        mrs     x9, mpidr_el1
        and     x10, x9, #0xff
        ubfx    x11, x9, #8, #1
        orr     x10, x10, x11, lsl #3           /* slot 0..15 */
        adrp    x12, sec_stacks
        add     x12, x12, :lo12:sec_stacks
        mov     x13, #0x1000
        madd    x12, x10, x13, x12
        add     x12, x12, x13                   /* top of this core's stack */
        msr     spsel, #1
        mov     sp, x12
        mov     x0, x10
        mov     x1, x9
        bl      sec_main
1:      wfe
        b       1b

/* Deterministic landing pad if EL1 ever faults, so we hang instead of
 * jumping to address 0. */
        .section .text.vectors, "ax"
        .balign 2048
        .globl  el1_vectors
el1_vectors:
        .rept   16
        b       .
        .balign 128
        .endr

        .bss
        .balign 16
        .space  0x8000
__stack:
        .space  16
        .globl  __stack_top
__stack_top:
