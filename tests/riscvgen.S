.text
.option norvc

addi zero, zero, 0

add a0, a1, a2
sub sp, ra, s4
addi a0, s6, -1
addi a0, s6, 2047

and a0, a1, a2
or sp, ra, s4
xor a0, a1, a2
andi a0, s6, -1
ori a0, s6, 2047
xori a0, s6, -1

sll a0, a1, a2
srl sp, ra, s4
sra a0, a1, a2
slli a0, a1, 1
srli a0, a1, 31
srai sp, ra, 7

slt a0, a1, a2
sltu sp, ra, s4
slti a0, s6, -1
sltiu a0, s6, 2047

lw a0, 16(a1)
lh a0, -16(a1)
lhu a0, 16(a1)
lb a0, -16(a1)
lbu a0, 16(a1)
sw a0, 16(a1)
sh a0, -16(a1)
sb a0, 16(a1)

beq a0, zero, 1f
addi zero, zero, 0
addi zero, zero, 0
addi zero, zero, 0
1:
addi zero, zero, 0
addi zero, zero, 0
addi zero, zero, 0
bne sp, zero, 1b

jal ra, 2f
addi zero, zero, 0
addi zero, zero, 0
addi zero, zero, 0
2:
jal zero, 3f
addi zero, zero, 0
addi zero, zero, 0
addi zero, zero, 0
3:
jalr a0, a1, 16

lui a0, 0x12345
auipc sp, 0xabcdf

mul a0, a1, a2
mulh sp, ra, s4
mulhu a0, a1, a2
div sp, ra, s4
divu a0, a1, a2
rem sp, ra, s4
remu a0, a1, a2

addi a0, a1, 0
xori sp, ra, -1
sub a0, zero, a1
addi a0, zero, -1
sltiu a0, a1, 1
sltu sp, zero, ra

ecall
ebreak
fence.i
fence

srli a0, a1, 5
slli t0, a1, 27
or a0, a0, t0
addi sp, ra, 0
andi t0, a2, 31
sub t1, zero, t0
andi t1, t1, 31
srl a0, a1, t0
sll t1, a1, t1
or a0, a0, t1

lui a0, 0x12345
addi a0, a0, 1656
lui sp, 0x1
addi sp, sp, -2048
lui a0, 0x0
addi a0, a0, -2048
