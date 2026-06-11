ENTRY(_start)

MEMORY
{
  RAM (rwx) : ORIGIN = 0x40800000, LENGTH = 64K
}

SECTIONS
{
  .text ORIGIN(RAM) + 0x1000 : ALIGN(4)
  {
    *(.text.start)
    *(.text .text.*)
    *(.rodata .rodata.*)
  } > RAM

  .data : ALIGN(4)
  {
    *(.sdata .sdata.*)
    *(.data .data.*)
  } > RAM

  .bss : ALIGN(4)
  {
    *(.sbss .sbss.*)
    *(.bss .bss.*)
    *(COMMON)
  } > RAM

  PROVIDE(__stack_top = ORIGIN(RAM) + LENGTH(RAM));
}
