#!/usr/bin/env python3
"""Disassemble JIT block hex dumps using riscv32-esp-elf-objdump."""

import subprocess, struct, sys, tempfile, os

OBJDUMP = os.path.expanduser(
    "~/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin/riscv32-esp-elf-objdump"
)

# Register mapping for annotation
RV_REG_NAMES = {
    0: "zero", 1: "ra", 2: "sp", 3: "gp", 4: "tp",
    5: "t0", 6: "t1", 7: "t2",
    8: "s0", 9: "s1", 10: "a0", 11: "a1", 12: "a2",
    13: "a3", 14: "a4", 15: "a5", 16: "a6", 17: "a7",
    18: "s2", 19: "s3", 20: "s4", 21: "s5", 22: "s6", 23: "s7",
    24: "s8", 25: "s9", 26: "s10", 27: "s11",
    28: "t3", 29: "t4", 30: "t5", 31: "t6"
}

GBA_REG_MAP = {
    "s0": "r0", "s1": "r1", "a3": "r2", "a4": "r3",
    "a5": "r4", "a6": "r5", "s2": "r6", "a7": "r7",
    "t4": "r8", "s3": "r9", "s4": "r10", "t5": "r11",
    "s5": "r12", "t6": "r13/sp", "s6": "r14/lr", "s7": "PC",
    "s8": "N_flag", "s9": "Z_flag", "s10": "C_flag", "s11": "V_flag",
    "gp": "reg_base", "tp": "reg_cycles", "t3": "reg_save0",
    "t0": "temp", "t1": "temp2", "t2": "temp3",
    "a0": "result/arg0", "a1": "arg1", "a2": "arg2"
}

# JIT block hex dumps from the user's output
blocks = {
    0x8: [
        0x00000008, 0x00000000,
        0x00000bb7, 0x008b8b93, 0xfff20213, 0x00025c63,
        0x00000537, 0x14050513, 0x480ac2b7, 0xd6c28293, 0x000280e7,
        0x01c0006f, 0x004b8513, 0x480ac2b7, 0xf8828293, 0x000280e7,
        0x00000140, 0x00000000, 0x00000bb7, 0x140b8b93, 0xff4f8e13,
        0x000002b7, 0xffc28293, 0x005e7e33, 0x000e0513, 0x000f0593,
        0x480ad2b7, 0xd2c28293, 0x000280e7, 0x004e0513, 0x000a8593,
        0x480ad2b7, 0xd2c28293, 0x000280e7, 0x008e0513, 0x004b8613,
        0x000b0593, 0xff4f8f93, 0x480ad2b7, 0xc3028293, 0x000280e7,
        0xffeb0513, 0x004b8593, 0x480ac2b7, 0x43828293, 0x000280e7,
        0x00050a93, 0x010b8593, 0x07858f13, 0x002a9513, 0x00af0533,
        0x00cb8593, 0x480ad2b7, 0x83828293, 0x000280e7, 0x00050a93,
        0x480ad2b7, 0xf5828293, 0x000280e7, 0x00050f13, 0xffcf8e13,
        0x000002b7, 0xffc28293, 0x005e7e33, 0x000e0513, 0x018b8613,
        0x000f0593, 0xffcf8f93, 0x480ad2b7, 0xc3028293, 0x000280e7,
        0x080f7f13, 0x01ff6f13, 0x000f0513, 0x020b8593, 0xf0000637,
        0x02060613, 0xf00002b7, 0x0ef28293, 0x480ad2b7, 0xf7028293,
        0x000280e7, 0xff8f8e13, 0x000002b7, 0xffc28293, 0x005e7e33,
        0x000e0513, 0x00068593, 0x480ad2b7, 0xd2c28293, 0x000280e7,
        0x004e0513, 0x028b8613, 0x000b0593, 0xff8f8f93, 0x480ad2b7,
        0xc3028293, 0x000280e7, 0x030b8593, 0x00058b13, 0x000a8513,
        0x480ac2b7, 0x2a828293, 0x000280e7, 0x030b8513, 0x480ac2b7,
        0xf8828293, 0x000280e7,
    ],
    0xbd4: [
        0x00000bd4, 0x00000000,
        0x00001bb7, 0xbd4b8b93, 0xfff20213, 0x020c8063,
        0x00025c63, 0x00001537, 0xc2450513, 0x480ac2b7, 0xd6c28293,
        0x000280e7, 0x37c0006f,
        0x009a5513, 0x00a48a33, 0x0186dd13, 0x001d7d13, 0x0196d513,
        0x00050693, 0x01f6dc13, 0x0016bc93, 0xffd20213,
        0x020d1063, 0x00025c63,
        0x00001537, 0xc1450513, 0x480ac2b7, 0xd6c28293, 0x000280e7,
        0x1700006f,
        0x00040513, 0x010b8593, 0x480ad2b7, 0x83828293, 0x000280e7,
        0x00050693, 0x00068713, 0x00068793, 0x00068813, 0x00068913,
        0x00068893, 0x00068e93, 0x00068993,
        0xff620213,
        0x00048393, 0x41448333, 0x0143bd33, 0x001d4d13, 0x0143c2b3,
        0x0063c333, 0x0062fdb3, 0x01fddd93, 0x01f35c13, 0x00133c93,
        0xffe20213,
        0x41bc02b3, 0x0a028e63,
        0x00048e13, 0x000002b7, 0xffc28293, 0x005e7e33,
        0x000e0513, 0x00068593, 0x480ad2b7, 0xd2c28293, 0x000280e7,
        0x004e0513, 0x00070593, 0x480ad2b7, 0xd2c28293, 0x000280e7,
        0x008e0513, 0x00078593, 0x480ad2b7, 0xd2c28293, 0x000280e7,
        0x00ce0513, 0x00080593, 0x480ad2b7, 0xd2c28293, 0x000280e7,
        0x010e0513, 0x00090593, 0x480ad2b7, 0xd2c28293, 0x000280e7,
        0x014e0513, 0x00088593, 0x480ad2b7, 0xd2c28293, 0x000280e7,
        0x018e0513, 0x000e8593, 0x480ad2b7, 0xd2c28293, 0x000280e7,
        0x01ce0513, 0x038b8613, 0x00098593, 0x02048493,
        0x480ad2b7, 0xc3028293, 0x000280e7,
        0xff720213, 0x41bc02b3, 0x02028063,
        0x00025c63, 0x00001537, 0xc0450513, 0x480ac2b7, 0xd6c28293,
        0x000280e7, 0xef1ff06f,
        0xfff20213, 0x00025c63,
        0x00001537, 0xc2450513, 0x480ac2b7, 0xd6c28293, 0x000280e7,
        0x1cc0006f,
        0x00048393, 0x41448333, 0x0143bd33, 0x001d4d13, 0x0143c2b3,
        0x0063c333, 0x0062fdb3,
    ],
}

def disasm_block(pc, words):
    """Disassemble a JIT block, skipping the 2-word header."""
    code_words = words[2:]  # skip header (pc, flags)
    raw = b"".join(struct.pack("<I", w) for w in code_words)
    
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(raw)
        tmp = f.name
    
    try:
        result = subprocess.run(
            [OBJDUMP, "-b", "binary", "-m", "riscv", "-M", "no-aliases",
             "-D", "--adjust-vma=0", tmp],
            capture_output=True, text=True
        )
        print(f"\n{'='*80}")
        print(f"JIT BLOCK at GBA PC = 0x{pc:x} ({len(raw)} bytes of code)")
        print(f"{'='*80}")
        
        for line in result.stdout.split("\n"):
            line = line.strip()
            if not line or line.startswith("Disassembly") or line.startswith(tmp) or line.startswith("..."):
                continue
            # Add GBA register annotations
            annotated = line
            for rv_name, gba_name in GBA_REG_MAP.items():
                if rv_name in line.split("#")[0]:  # don't annotate comments
                    pass  # handled below
            print(f"  {line}")
    finally:
        os.unlink(tmp)

for pc in sorted(blocks.keys()):
    disasm_block(pc, blocks[pc])
