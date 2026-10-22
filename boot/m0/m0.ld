/* LK copies boot_b's kernel to 0x80080000 and jumps there, so link there. */
ENTRY(_image_start)
SECTIONS
{
. = 0x80080000;
.text : {
KEEP(*(.text.header))
*(.text .text.*)
}
.rodata : { *(.rodata .rodata.*) }
.data : { *(.data .data.*) }
.bss (NOLOAD) : {
__bss_start = .;
*(.bss .bss.*) *(COMMON)
__bss_end = .;
}
/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
. = ALIGN(16);
__image_end = .;
__image_size = __image_end - _image_start;
}
