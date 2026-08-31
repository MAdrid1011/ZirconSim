.option norvc
.section .text.entry, "ax"
.globl _start
_start:
  addi a0, zero, 5
  addi a1, a0, -2
  add a2, a0, a1
  slli a3, a2, 1
  slti a4, a3, 7
  beq a4, zero, 1f
  addi a5, zero, 99
1:
  auipc a5, 0
  jal a6, 2f
  addi a7, zero, 99
2:
  xor a7, a6, a5
  srli a7, a7, 2
  csrrw zero, mscratch, a7
  csrrs t0, mscratch, zero
  csrrsi t1, mscratch, 1
  csrrci t2, mscratch, 1
  lui t3, %hi(tohost)
  addi t3, t3, %lo(tohost)
  addi t4, zero, 1
  sw t4, 0(t3)
3:
  jal zero, 3b
