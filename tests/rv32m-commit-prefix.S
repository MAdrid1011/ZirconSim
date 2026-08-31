.option norvc
.section .text.entry, "ax"
.globl _start
_start:
  addi t0, zero, -7
  addi t1, zero, 3
  lui t2, 0x80000
  addi t3, zero, -1
  mul t4, t0, t1
  mulh t5, t2, t1
  mulhsu t6, t0, t3
  mulhu a0, t3, t3
  div a1, t0, t1
  divu a2, t3, zero
  rem a3, t0, t1
  remu a4, t0, zero
  div a5, t2, t3
  rem a6, t2, t3
  lui a7, %hi(tohost)
  addi a7, a7, %lo(tohost)
  addi s0, zero, 1
  sw s0, 0(a7)
1:
  jal zero, 1b
