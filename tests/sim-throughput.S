.option norvc
.section .text.entry, "ax"
.globl _start
_start:
  addi t0, zero, 0
loop:
  addi t0, t0, 1
  xori t1, t0, 85
  add t2, t0, t1
  beq zero, zero, loop
