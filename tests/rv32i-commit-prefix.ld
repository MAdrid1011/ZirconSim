OUTPUT_ARCH("riscv")
ENTRY(_start)

SECTIONS {
  . = 0x80000000;
  .text : {
    *(.text.entry)
    *(.text*)
  }
  .tohost : ALIGN(64) {
    tohost = .;
    LONG(0);
    fromhost = .;
    LONG(0);
  }
}
