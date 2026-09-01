.option norvc
.section .text.entry, "ax"
.globl _start
_start:
  lui t0, %hi(atomic_word)
  addi t0, t0, %lo(atomic_word)
  addi t1, zero, 5
  amoadd.w a0, t1, (t0)
  lr.w a1, (t0)
  addi t2, zero, 9
  sc.w a2, t2, (t0)
  lui a3, %hi(tohost)
  addi a3, a3, %lo(tohost)
  addi a4, zero, 1
  sw a4, 0(a3)
1:
  jal zero, 1b

.section .tohost, "aw"
.align 2
atomic_word:
  .word 7
