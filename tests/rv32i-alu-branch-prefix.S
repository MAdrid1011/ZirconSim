.option norvc
.section .text.entry, "ax"
.globl _start
_start:
  addi t0, zero, -16
  addi t1, zero, 3
  sll t2, t1, t1
  slt t3, t0, t1
  sltu t4, t0, t1
  sub t5, t1, t0
  xor t6, t5, t2
  srl a0, t0, t1
  sra a1, t0, t1
  or a2, t3, t4
  and a3, t2, t5
  addi a4, t1, -7
  slti a5, t0, -15
  sltiu a6, t1, -1
  xori a7, a3, -1
  ori s0, a3, 3
  andi s1, s0, 17
  slli s2, s1, 2
  srli s3, s2, 2
  srai s4, t0, 2
  bne s4, a4, fail
  blt t0, t1, 1f
  addi s5, zero, 99
1:
  bge t1, t0, 2f
  addi s6, zero, 99
2:
  bltu t1, t0, 3f
  addi s7, zero, 99
3:
  bgeu t0, t1, 4f
  addi s8, zero, 99
4:
  beq s1, s3, 5f
  addi s9, zero, 99
5:
  auipc s10, 0
  addi s10, s10, 16
  jalr zero, s10, 0
  addi s10, zero, 99
6:
  lui s11, %hi(tohost)
  addi s11, s11, %lo(tohost)
  addi t0, zero, 1
  sw t0, 0(s11)
7:
  jal zero, 7b
fail:
  jal zero, fail
