
ARMV8PFX=/opt/buildroot-armv8el-uclibc/bin/aarch64-buildroot-linux-uclibc
MIPS32PFX=/opt/buildroot-mipsel32-o32-uclibc/bin/mipsel-buildroot-linux-uclibc
RISCV32CC ?= riscv32-esp-elf-gcc
RISCV32OBJCOPY ?= riscv32-esp-elf-objcopy
RISCV32NM ?= riscv32-esp-elf-nm
HOSTCC ?= gcc

.PHONY: all riscv riscv-check riscv-emit-check riscv-smoke riscv-stub-check riscv-all

all:
	$(HOSTCC) -o arm64gen arm64gen.c -ggdb -I../arm/
	./arm64gen > bytecode.bin
	$(ARMV8PFX)-as -o bytecoderef.o arm64gen.S
	$(ARMV8PFX)-objcopy -O binary bytecoderef.o bytecoderef.bin
	@ cmp bytecoderef.bin bytecode.bin || echo "Bytecode mismatch"
	@ cmp bytecoderef.bin bytecode.bin && echo "Test passed!"
	$(HOSTCC) -o mipsgen mipsgen.c -ggdb -I../mips/
	./mipsgen > bytecode.bin
	$(MIPS32PFX)-as -EL -o bytecoderef.o mipsgen.S
	$(MIPS32PFX)-objcopy -j .text -O binary bytecoderef.o bytecoderef.bin
	@ cmp bytecoderef.bin bytecode.bin || echo "Bytecode mismatch"
	@ cmp bytecoderef.bin bytecode.bin && echo "Test passed!"
	$(MAKE) riscv
	$(MAKE) riscv-smoke

riscv-all:
	$(MAKE) riscv
	$(MAKE) riscv-smoke

riscv:
	$(HOSTCC) -o riscvgen riscvgen.c -ggdb -I.. -I../riscv/ -I../libretro/libretro-common/include
	./riscvgen > bytecode.bin
	$(RISCV32CC) -c riscvgen.S -o bytecoderef.o
	$(RISCV32OBJCOPY) -j .text -O binary bytecoderef.o bytecoderef.bin
	@ cmp bytecoderef.bin bytecode.bin || echo "Bytecode mismatch"
	@ cmp bytecoderef.bin bytecode.bin && echo "RISC-V codegen test passed!"
	$(MAKE) riscv-check
	$(MAKE) riscv-emit-check
	$(MAKE) riscv-stub-check

riscv-smoke:
	$(RISCV32CC) -DRISCV_ARCH -I.. -I../libretro/libretro-common/include -c ../cpu_threaded.c -o cpu_threaded-riscv.o
	$(RISCV32CC) -c ../riscv/riscv_stub.S -o riscv_stub.o
	@ echo "RISC-V backend smoke test passed!"

riscv-stub-check:
	$(RISCV32CC) -c ../riscv/riscv_stub.S -o riscv_stub.o
	@for sym in \
		execute_arm_translate_internal \
		rv_update_gba \
		rv_indirect_branch_arm \
		rv_indirect_branch_thumb \
		rv_indirect_branch_dual \
		rv_execute_load_u8 \
		rv_execute_load_s8 \
		rv_execute_load_u16 \
		rv_execute_load_s16 \
		rv_execute_load_u32 \
		rv_execute_aligned_load32 \
		rv_execute_store_u8 \
		rv_execute_store_u16 \
		rv_execute_store_u32 \
		rv_execute_aligned_store32 \
		execute_read_cpsr \
		execute_read_spsr \
		execute_store_cpsr \
		execute_store_spsr \
		execute_spsr_restore \
		execute_swi \
		rv_cheat_hook \
		jit_call_c_trampoline; do \
		$(RISCV32NM) riscv_stub.o | grep -Eq " [Tt] $$sym$$" || { echo "Missing symbol: $$sym"; exit 1; }; \
	done
	@echo "RISC-V stub symbol check passed!"

riscv-check:
	$(HOSTCC) -o riscv_codegen_check riscv_codegen_check.c -ggdb -I.. -I../riscv/ -I../libretro/libretro-common/include
	./riscv_codegen_check

riscv-emit-check:
	$(HOSTCC) -o riscv_emit_check riscv_emit_check.c -ggdb -I.. -I../riscv/ -I../libretro/libretro-common/include
	./riscv_emit_check


