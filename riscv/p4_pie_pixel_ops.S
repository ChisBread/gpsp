.text
.align 4

.global gpsp_p4_fill_u16_pie
.type gpsp_p4_fill_u16_pie, @function
gpsp_p4_fill_u16_pie:
    addi sp, sp, -16

    sh a2,  0(sp)
    sh a2,  2(sp)
    sh a2,  4(sp)
    sh a2,  6(sp)
    sh a2,  8(sp)
    sh a2, 10(sp)
    sh a2, 12(sp)
    sh a2, 14(sp)

    mv a3, sp
    esp.vld.128.ip q0, a3, 16

.Lfill_loop:
    beqz a1, .Lfill_done
    esp.vst.128.ip q0, a0, 16
    addi a1, a1, -8
    j .Lfill_loop

.Lfill_done:
    addi sp, sp, 16
    ret

.size gpsp_p4_fill_u16_pie, .-gpsp_p4_fill_u16_pie

.global gpsp_p4_fill_u32_pie
.type gpsp_p4_fill_u32_pie, @function
gpsp_p4_fill_u32_pie:
    addi sp, sp, -16

    sw a2,  0(sp)
    sw a2,  4(sp)
    sw a2,  8(sp)
    sw a2, 12(sp)

    mv a3, sp
    esp.vld.128.ip q0, a3, 16

.Lfill32_loop:
    beqz a1, .Lfill32_done
    esp.vst.128.ip q0, a0, 16
    addi a1, a1, -4
    j .Lfill32_loop

.Lfill32_done:
    addi sp, sp, 16
    ret

.size gpsp_p4_fill_u32_pie, .-gpsp_p4_fill_u32_pie