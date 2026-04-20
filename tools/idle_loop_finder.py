#!/usr/bin/env python3
"""
GBA Idle Loop Finder — Static + Dynamic analysis tool.

Finds idle_loop_target_pc candidates in GBA ROMs by:
  1. Static: scanning for backward branch patterns with IO-polling loop bodies
  2. Dynamic: generating a profiler patch for gpsp to detect hot branches at runtime

Usage:
  python idle_loop_finder.py <rom.gba>              # Analyze single ROM
  python idle_loop_finder.py --batch <rom_dir>       # Batch analyze all .gba in dir
  python idle_loop_finder.py --dynamic-patch         # Print C profiler patch
  python idle_loop_finder.py --verify <rom.gba> 0x80031d6  # Verify known address

How idle_loop_target_pc works in gpsp:
  The interpreter checks `if (reg[REG_PC] == idle_loop_target_pc)` after each
  instruction. REG_PC points to the NEXT instruction. So idle_loop_target_pc is
  the address of the backward branch itself: when PC reaches it, cycles are zeroed,
  the execution loop exits before executing the branch, and the emulator fast-forwards
  to the next event (typically VBlank interrupt).
"""

import struct
import sys
import os
import glob
import argparse
from collections import defaultdict
from dataclasses import dataclass, field
from typing import List, Optional, Tuple, Dict, Set

# ─── GBA Constants ───────────────────────────────────────────────────────────

ROM_BASE = 0x08000000
IO_BASE  = 0x04000000
IO_END   = 0x04000400

# IO registers commonly polled in idle loops
IO_POLL_REGS = {
    0x04000004: "DISPSTAT",
    0x04000006: "VCOUNT",
    0x04000130: "KEYINPUT",
    0x04000200: "IE",
    0x04000202: "IF",
    0x04000208: "IME",
}

# High-value status registers (strong idle loop signal)
IO_STATUS_REGS = {"DISPSTAT", "VCOUNT", "IF", "IE"}

# GBA memory regions
IWRAM_BASE = 0x03000000
IWRAM_END  = 0x03008000
EWRAM_BASE = 0x02000000
EWRAM_END  = 0x02040000

COND_NAMES = [
    "eq","ne","cs","cc","mi","pl","vs","vc",
    "hi","ls","ge","lt","gt","le","al","nv"
]

# ─── Data Classes ────────────────────────────────────────────────────────────

@dataclass
class IdleCandidate:
    pc: int                     # Address of the backward branch instruction
    target: int                 # Branch target (loop start)
    loop_size: int              # Number of instructions in loop body
    mode: str                   # "ARM" or "Thumb"
    confidence: float           # 0.0 to 1.0
    io_reads: List[str]         # IO register names read in the loop
    reasons: List[str]          # Scoring reasons
    instructions: List[str]     # Disassembled instructions
    alt_pcs: List[int] = None   # Other PCs in the loop body (for matching)

    def __post_init__(self):
        if self.alt_pcs is None:
            self.alt_pcs = []

    def matches_pc(self, expected_pc: int) -> bool:
        """Check if this candidate matches the expected idle loop PC."""
        return self.pc == expected_pc or expected_pc in self.alt_pcs

# ─── Thumb Instruction Decoder ───────────────────────────────────────────────

class ThumbDec:
    """Minimal Thumb-mode instruction decoder for idle loop analysis."""

    @staticmethod
    def cond_branch(instr: int) -> Optional[Tuple[int, int]]:
        """Conditional branch? Returns (condition, pc_relative_offset) or None.
        Target = branch_pc + returned_offset."""
        if (instr >> 12) != 0xD:
            return None
        cond = (instr >> 8) & 0xF
        if cond >= 0xE:  # 0xE=undefined, 0xF=SWI format
            return None
        off = instr & 0xFF
        if off & 0x80:
            off -= 0x100
        # Thumb pipeline: PC+4, offset is in halfwords
        return (cond, off * 2 + 4)

    @staticmethod
    def uncond_branch(instr: int) -> Optional[int]:
        """Unconditional B? Returns pc-relative offset or None."""
        if (instr >> 11) != 0b11100:
            return None
        off = instr & 0x7FF
        if off & 0x400:
            off -= 0x800
        return off * 2 + 4

    @staticmethod
    def is_store(instr: int) -> bool:
        t5 = (instr >> 11) & 0x1F
        t7 = (instr >> 9) & 0x7F
        return t7 in (0b0101000,   # STR  Rd,[Rb,Ro]
                       0b0101010,   # STRB Rd,[Rb,Ro]
                       0b0101001,   # STRH Rd,[Rb,Ro]
                       0b1011010,   # PUSH {rlist}
                       ) or \
               t5 in (0b01100,     # STR  Rd,[Rb,#imm5]
                      0b01110,     # STRB Rd,[Rb,#imm5]
                      0b10000,     # STRH Rd,[Rb,#imm5]
                      0b10010,     # STR  Rd,[SP,#imm8]
                      0b11000,     # STMIA Rb!,{rlist}
                      )

    @staticmethod
    def is_bl_part(instr: int) -> bool:
        """Is this part of a BL/BLX two-instruction sequence?"""
        top5 = (instr >> 11) & 0x1F
        return top5 in (0b11110, 0b11111, 0b11101)

    @staticmethod
    def is_func_call(instr: int) -> bool:
        """BL suffix or BLX Rm?"""
        if (instr >> 11) in (0b11111, 0b11101):
            return True
        # BLX Rm: 010001 111 Rm 000
        if (instr & 0xFF80) == 0x4780:
            return True
        return False

    @staticmethod
    def is_swi(instr: int) -> bool:
        return (instr >> 8) == 0xDF

    @staticmethod
    def is_cmp(instr: int) -> bool:
        """CMP Rd, #imm8 or CMP Rd, Rm (ALU format 4)."""
        if (instr >> 11) == 0b00101:  # CMP Rd, #imm8
            return True
        if (instr >> 10) == 0b010000 and ((instr >> 6) & 0xF) == 10:  # CMP in ALU
            return True
        return False

    @staticmethod
    def is_tst(instr: int) -> bool:
        """TST Rd, Rm (ALU format 4)."""
        return (instr >> 10) == 0b010000 and ((instr >> 6) & 0xF) == 8

    @staticmethod
    def is_bl(instr: int) -> bool:
        """Is this the suffix of a BL/BLX sequence, or BLX Rm?"""
        if (instr >> 11) in (0b11111, 0b11101):  # BL/BLX suffix
            return True
        if (instr & 0xFF80) == 0x4780:  # BLX Rm
            return True
        return False

    @staticmethod
    def is_bx(instr: int) -> bool:
        """BX Rm (not BLX)."""
        return (instr & 0xFF80) == 0x4700

    @staticmethod
    def pc_rel_load(instr: int, pc: int) -> Optional[Tuple[int, int]]:
        """LDR Rd,[PC,#imm]? Returns (Rd, load_address)."""
        if (instr >> 11) != 0b01001:
            return None
        rd  = (instr >> 8) & 0x7
        imm = (instr & 0xFF) << 2
        addr = ((pc + 4) & ~3) + imm
        return (rd, addr)

    @staticmethod
    def reg_load(instr: int) -> Optional[Tuple[int, int, int]]:
        """Load from [Rb, #offset]? Returns (Rd, Rb, byte_offset) or None."""
        t5 = (instr >> 11) & 0x1F
        t7 = (instr >> 9) & 0x7F
        rd = instr & 0x7
        rb = (instr >> 3) & 0x7
        imm5 = (instr >> 6) & 0x1F
        # Format 9: LDR/LDRB with imm5
        if t5 == 0b01101:  # LDR Rd,[Rb,#imm5*4]
            return (rd, rb, imm5 * 4)
        if t5 == 0b01111:  # LDRB Rd,[Rb,#imm5]
            return (rd, rb, imm5)
        if t5 == 0b10001:  # LDRH Rd,[Rb,#imm5*2]
            return (rd, rb, imm5 * 2)
        # Format 7/8: register-offset loads (offset unknown statically)
        if t7 in (0b0101100, 0b0101110, 0b0101101, 0b0101011, 0b0101111):
            return (rd, rb, -1)  # -1 = unknown offset
        return None

    @staticmethod
    def add_reg_imm3(instr: int) -> Optional[Tuple[int, int, int]]:
        """ADD Rd, Rs, #imm3 or SUB Rd, Rs, #imm3? Returns (Rd, Rs, imm3) or None."""
        if (instr >> 9) == 0b0001110:  # ADD
            return (instr & 7, (instr >> 3) & 7, (instr >> 6) & 7)
        if (instr >> 9) == 0b0001111:  # SUB
            return (instr & 7, (instr >> 3) & 7, -((instr >> 6) & 7))
        return None

    @staticmethod
    def add_reg_reg(instr: int) -> Optional[Tuple[int, int, int]]:
        """ADD/SUB Rd, Rs, Rn (format 2)? Returns (Rd, Rs, Rn) or None."""
        if (instr >> 9) == 0b0001100:  # ADD Rd, Rs, Rn
            return (instr & 7, (instr >> 3) & 7, (instr >> 6) & 7)
        return None

    @staticmethod
    def is_pure_alu(instr: int) -> bool:
        """Is this a pure ALU/data-processing instruction (no memory access)?"""
        t5 = (instr >> 11) & 0x1F
        t4 = (instr >> 12) & 0xF
        # Format 4: ALU operations
        if (instr >> 10) == 0b010000: return True
        # Format 3: MOV/CMP/ADD/SUB Rd, #imm8
        if t5 in (0b00100, 0b00101, 0b00110, 0b00111): return True
        # Format 1: LSL/LSR/ASR
        if t5 in (0b00000, 0b00001, 0b00010): return True
        # Format 2: ADD/SUB reg/imm3
        if (instr >> 11) in (0b00011,): return True
        # Format 5: Hi-reg MOV/CMP/ADD (but NOT BX/BLX)
        if (instr >> 10) == 0b010001:
            op = (instr >> 8) & 3
            if op != 3:  # not BX/BLX
                return True
        return False

    @staticmethod
    def mov_imm(instr: int) -> Optional[Tuple[int, int]]:
        """MOV Rd, #imm8? Returns (Rd, imm)."""
        if (instr >> 11) == 0b00100:
            return ((instr >> 8) & 0x7, instr & 0xFF)
        return None

    @staticmethod
    def lsl_imm(instr: int) -> Optional[Tuple[int, int, int]]:
        """LSL Rd, Rs, #imm5? Returns (Rd, Rs, shift)."""
        if (instr >> 11) == 0b00000:
            shift = (instr >> 6) & 0x1F
            rs = (instr >> 3) & 0x7
            rd = instr & 0x7
            if shift > 0:  # shift=0 is MOV Rd,Rs
                return (rd, rs, shift)
        return None

    @staticmethod
    def add_imm(instr: int) -> Optional[Tuple[int, int]]:
        """ADD Rd, #imm8? Returns (Rd, imm)."""
        if (instr >> 11) == 0b00110:
            return ((instr >> 8) & 0x7, instr & 0xFF)
        return None

    @staticmethod
    def disasm(instr: int, pc: int) -> str:
        """Simple disassembly for display."""
        cb = ThumbDec.cond_branch(instr)
        if cb:
            cond, off = cb
            return f"{COND_NAMES[cond]:4s} b  0x{pc+off:08x}"

        ub = ThumbDec.uncond_branch(instr)
        if ub is not None:
            return f"     b  0x{pc+ub:08x}"

        pcl = ThumbDec.pc_rel_load(instr, pc)
        if pcl:
            rd, addr = pcl
            return f"     ldr  r{rd}, [pc, #...] ; =0x{addr:08x}"

        rl = ThumbDec.reg_load(instr)
        if rl:
            rd, rb, boff = rl
            t5 = (instr >> 11) & 0x1F
            ltype = {0b01101:"ldr", 0b01111:"ldrb", 0b10001:"ldrh"}.get(t5, "ldr?")
            if boff >= 0:
                return f"     {ltype} r{rd}, [r{rb}, #{boff}]"
            return f"     {ltype} r{rd}, [r{rb}, ...]"  # register offset

        mi = ThumbDec.mov_imm(instr)
        if mi:
            rd, imm = mi
            return f"     mov  r{rd}, #0x{imm:x}"

        li = ThumbDec.lsl_imm(instr)
        if li:
            rd, rs, sh = li
            return f"     lsl  r{rd}, r{rs}, #{sh}"

        ai = ThumbDec.add_imm(instr)
        if ai:
            rd, imm = ai
            return f"     add  r{rd}, #0x{imm:x}"

        ari = ThumbDec.add_reg_imm3(instr)
        if ari:
            rd, rs, imm = ari
            if imm >= 0:
                return f"     add  r{rd}, r{rs}, #{imm}"
            else:
                return f"     sub  r{rd}, r{rs}, #{-imm}"

        if ThumbDec.is_store(instr):   return "     str/push"
        if ThumbDec.is_swi(instr):     return f"     swi  #{instr & 0xFF}"
        if ThumbDec.is_func_call(instr): return "     bl/blx"
        if ThumbDec.is_bx(instr):      return f"     bx   r{(instr>>3)&0xF}"

        # ALU ops (format 4): 010000 op Rs Rd
        if (instr >> 10) == 0b010000:
            op = (instr >> 6) & 0xF
            ops = ["and","eor","lsl","lsr","asr","adc","sbc","ror",
                   "tst","neg","cmp","cmn","orr","mul","bic","mvn"]
            return f"     {ops[op]:4s} r{instr&7}, r{(instr>>3)&7}"

        # CMP Rd, #imm8
        if (instr >> 11) == 0b00101:
            return f"     cmp  r{(instr>>8)&7}, #0x{instr&0xFF:x}"

        # SUB Rd, #imm8
        if (instr >> 11) == 0b00111:
            return f"     sub  r{(instr>>8)&7}, #0x{instr&0xFF:x}"

        # Format 1: LSR/ASR with imm5
        t5 = (instr >> 11) & 0x1F
        if t5 == 0b00001:  # LSR
            return f"     lsr  r{instr&7}, r{(instr>>3)&7}, #{(instr>>6)&0x1F}"
        if t5 == 0b00010:  # ASR
            return f"     asr  r{instr&7}, r{(instr>>3)&7}, #{(instr>>6)&0x1F}"

        # Format 1 shift=0: MOV Rd, Rs
        if t5 == 0b00000 and ((instr >> 6) & 0x1F) == 0:
            return f"     mov  r{instr&7}, r{(instr>>3)&7}"

        # POP/PUSH
        if (instr >> 9) == 0b1011110:  # POP
            return f"     pop  {{...}}"
        if (instr >> 9) == 0b1011010:  # PUSH
            return f"     push {{...}}"

        return f"     ??? (0x{instr:04x})"


# ─── ARM Instruction Decoder ────────────────────────────────────────────────

class ARMDec:
    """Minimal ARM-mode instruction decoder for idle loop analysis."""

    @staticmethod
    def branch(instr: int) -> Optional[Tuple[int, bool, int]]:
        """B/BL? Returns (condition, is_link, pc_relative_offset)."""
        if ((instr >> 25) & 0x7) != 0b101:
            return None
        cond = (instr >> 28) & 0xF
        is_link = bool((instr >> 24) & 1)
        off = instr & 0x00FFFFFF
        if off & 0x800000:
            off |= 0xFF000000
        off = struct.unpack('<i', struct.pack('<I', off & 0xFFFFFFFF))[0]
        return (cond, is_link, off * 4 + 8)

    @staticmethod
    def is_store(instr: int) -> bool:
        # Single data transfer store: [27:26]=01, bit20(L)=0
        if (instr >> 26) & 3 == 1 and not ((instr >> 20) & 1):
            return True
        # Block data transfer store: [27:25]=100, bit20(L)=0
        if (instr >> 25) & 7 == 4 and not ((instr >> 20) & 1):
            return True
        # Misc halfword store: xxxx 0000 xx0x xxxx xxxx xxxx 1011 xxxx (STRH)
        if (instr & 0x0E4000F0) == 0x000000B0 and not ((instr >> 20) & 1):
            return True
        return False

    @staticmethod
    def is_swi(instr: int) -> bool:
        return ((instr >> 24) & 0xF) == 0xF

    @staticmethod
    def pc_rel_load(instr: int, pc: int) -> Optional[Tuple[int, int]]:
        """LDR Rd,[PC,#imm]? Returns (Rd, load_address)."""
        # Must be: single data transfer, Rn=PC(15), L=1(load), I=0(immediate)
        if (instr & 0x0F7F0000) != 0x051F0000:
            return None
        rd = (instr >> 12) & 0xF
        imm = instr & 0xFFF
        if (instr >> 23) & 1:  # U bit: add
            addr = pc + 8 + imm
        else:
            addr = pc + 8 - imm
        return (rd, addr)

    @staticmethod
    def get_load_base(instr: int) -> Optional[int]:
        """For load instructions, return the base register Rn."""
        # Single data transfer load
        if (instr >> 26) & 3 == 1 and ((instr >> 20) & 1):
            return (instr >> 16) & 0xF
        # Misc halfword load
        if (instr & 0x0E000090) == 0x00000090 and ((instr >> 20) & 1):
            return (instr >> 16) & 0xF
        return None

    @staticmethod
    def disasm(instr: int, pc: int) -> str:
        cond = COND_NAMES[(instr >> 28) & 0xF]
        br = ARMDec.branch(instr)
        if br:
            c, link, off = br
            bl = "bl" if link else "b"
            return f"{cond:4s} {bl}  0x{pc+off:08x}"
        pcl = ARMDec.pc_rel_load(instr, pc)
        if pcl:
            rd, addr = pcl
            return f"{cond:4s} ldr  r{rd}, [pc, #...] ; =0x{addr:08x}"
        base = ARMDec.get_load_base(instr)
        if base is not None:
            rd = (instr >> 12) & 0xF
            return f"{cond:4s} ldr  r{rd}, [r{base}, ...]"
        if ARMDec.is_store(instr): return f"{cond:4s} str/stm"
        if ARMDec.is_swi(instr):   return f"{cond:4s} swi"
        # Data processing
        if (instr >> 26) & 3 == 0:
            op = (instr >> 21) & 0xF
            ops = ["and","eor","sub","rsb","add","adc","sbc","rsc",
                   "tst","teq","cmp","cmn","orr","mov","bic","mvn"]
            rd = (instr >> 12) & 0xF
            rn = (instr >> 16) & 0xF
            return f"{cond:4s} {ops[op]}  r{rd}, r{rn}, ..."
        return f"     ??? (0x{instr:08x})"


# ─── Loop Analyzer ───────────────────────────────────────────────────────────

class IdleLoopFinder:
    MAX_LOOP_INSTRS = 20
    MIN_LOOP_INSTRS = 2

    def __init__(self, rom: bytes):
        self.rom = rom
        self.size = len(rom)
        self.candidates: List[IdleCandidate] = []

    def r16(self, off: int) -> int:
        if off + 2 > self.size: return 0
        return struct.unpack_from('<H', self.rom, off)[0]

    def r32(self, off: int) -> int:
        if off + 4 > self.size: return 0
        return struct.unpack_from('<I', self.rom, off)[0]

    def off(self, addr: int) -> int:
        return addr - ROM_BASE

    # ── Pre-loop context scanning ──

    def _scan_thumb_precontext(self, target_pc: int, max_back: int = 20) -> Dict[int, int]:
        """Scan instructions before the loop to find register constants.
        Many idle loops use a base register set before the loop."""
        regs: Dict[int, int] = {}
        # Scan up to max_back instructions before loop start
        start_pc = max(ROM_BASE, target_pc - max_back * 2)
        pc = start_pc
        while pc < target_pc:
            o = self.off(pc)
            if o < 0 or o + 2 > self.size:
                pc += 2
                continue
            hw = self.r16(o)

            # PC-relative loads
            pcl = ThumbDec.pc_rel_load(hw, pc)
            if pcl:
                rd, addr = pcl
                pool_off = self.off(addr)
                if 0 <= pool_off <= self.size - 4:
                    regs[rd] = self.r32(pool_off)

            # MOV Rd, #imm
            mi = ThumbDec.mov_imm(hw)
            if mi:
                rd, imm = mi
                regs[rd] = imm

            # LSL Rd, Rs, #imm
            li = ThumbDec.lsl_imm(hw)
            if li:
                rd, rs, shift = li
                if rs in regs:
                    regs[rd] = (regs[rs] << shift) & 0xFFFFFFFF

            # ADD Rd, #imm8
            ai = ThumbDec.add_imm(hw)
            if ai:
                rd, imm = ai
                if rd in regs:
                    regs[rd] = (regs[rd] + imm) & 0xFFFFFFFF

            # Function call/BX resets our tracking (new scope)
            if ThumbDec.is_func_call(hw) or ThumbDec.is_bx(hw):
                regs.clear()

            pc += 2
        return regs

    # ── Thumb analysis ──

    def _analyze_thumb_loop(self, branch_pc: int, target_pc: int) -> Optional[IdleCandidate]:
        n = (branch_pc - target_pc) // 2 + 1
        if not (self.MIN_LOOP_INSTRS <= n <= self.MAX_LOOP_INSTRS):
            return None

        # Get register context from before the loop
        pre_regs = self._scan_thumb_precontext(target_pc)
        regs: Dict[int, int] = dict(pre_regs)  # start with pre-loop context

        has_store = has_call = has_swi = has_bx = False
        has_load = False
        has_cmp_or_tst = False
        has_forward_branch = False  # conditional branch that exits the loop
        io_reads: List[str] = []
        ram_reads: List[str] = []
        instrs: List[str] = []
        load_count = 0
        alu_count = 0

        pc = target_pc
        while pc <= branch_pc:
            o = self.off(pc)
            if o < 0 or o + 2 > self.size:
                return None
            hw = self.r16(o)

            instrs.append(f"  0x{pc:08X}: {hw:04x}  {ThumbDec.disasm(hw, pc)}")

            if ThumbDec.is_store(hw):     has_store = True
            if ThumbDec.is_func_call(hw): has_call = True
            if ThumbDec.is_swi(hw):       has_swi = True
            if ThumbDec.is_bx(hw):        has_bx = True
            if ThumbDec.is_pure_alu(hw):  alu_count += 1

            # Check for CMP/TST (strong idle loop indicator when combined with load)
            if (hw >> 10) == 0b010000:  # Format 4 ALU
                op = (hw >> 6) & 0xF
                if op in (8, 10):  # TST, CMP
                    has_cmp_or_tst = True
            if (hw >> 11) == 0b00101:  # CMP Rd, #imm8
                has_cmp_or_tst = True

            # Check for forward conditional branches (loop exit paths)
            cb = ThumbDec.cond_branch(hw)
            if cb and pc < branch_pc:
                _, rel = cb
                if rel > 0:  # forward = exit branch
                    has_forward_branch = True

            # Track PC-relative loads (literal pool constants)
            pcl = ThumbDec.pc_rel_load(hw, pc)
            if pcl:
                rd, addr = pcl
                pool_off = self.off(addr)
                if 0 <= pool_off <= self.size - 4:
                    regs[rd] = self.r32(pool_off)

            # Track MOV Rd, #imm
            mi = ThumbDec.mov_imm(hw)
            if mi:
                rd, imm = mi
                regs[rd] = imm

            # Track LSL Rd, Rs, #imm (for address construction)
            li = ThumbDec.lsl_imm(hw)
            if li:
                rd, rs, shift = li
                if rs in regs:
                    regs[rd] = (regs[rs] << shift) & 0xFFFFFFFF

            # Track ADD Rd, #imm8
            ai = ThumbDec.add_imm(hw)
            if ai:
                rd, imm = ai
                if rd in regs:
                    regs[rd] = (regs[rd] + imm) & 0xFFFFFFFF

            # Track ADD Rd, Rs, #imm3
            ari = ThumbDec.add_reg_imm3(hw)
            if ari:
                rd, rs, imm = ari
                if rs in regs:
                    regs[rd] = (regs[rs] + imm) & 0xFFFFFFFF

            # Check if this is a load from a known address
            rl = ThumbDec.reg_load(hw)
            if rl:
                rd, rb, boff = rl
                has_load = True
                load_count += 1
                if rb in regs:
                    if boff >= 0:
                        addr = (regs[rb] + boff) & 0xFFFFFFFF
                    else:
                        addr = regs[rb]  # approximate
                    if IO_BASE <= addr < IO_END:
                        name = IO_POLL_REGS.get(addr & ~1, f"IO:0x{addr:08X}")
                        if name not in io_reads:
                            io_reads.append(name)
                    elif IWRAM_BASE <= addr < IWRAM_END:
                        name = f"IWRAM:0x{addr:08X}"
                        if name not in ram_reads:
                            ram_reads.append(name)
                    elif EWRAM_BASE <= addr < EWRAM_END:
                        name = f"EWRAM:0x{addr:08X}"
                        if name not in ram_reads:
                            ram_reads.append(name)

            # BL prefix: skip (it's data for the BL suffix)
            if ThumbDec.is_bl_part(hw) and (hw >> 11) == 0b11110:
                pc += 2
                continue

            pc += 2

        # ── Reject hard disqualifiers ──
        if has_bx:
            return None

        # ── Score ──
        conf = 0.0
        reasons = []

        # Penalize (but don't reject) stores and calls
        if has_store:
            conf -= 0.35
            reasons.append("store_in_loop(-0.35)")
        if has_call:
            conf -= 0.2
            reasons.append("call_in_loop(-0.2)")
        if has_swi:
            conf -= 0.3
            reasons.append("swi_in_loop(-0.3)")

        # IO register polling (strongest signal)
        if io_reads:
            conf += 0.5
            reasons.append(f"io_read(+0.5): {','.join(io_reads)}")
        if any(r in IO_STATUS_REGS for r in io_reads):
            conf += 0.3
            reasons.append("status_reg(+0.3)")

        # RAM polling (common pattern: interrupt handler sets flag in RAM)
        if ram_reads and not io_reads:
            conf += 0.4
            reasons.append(f"ram_poll(+0.4): {','.join(ram_reads)}")

        # Pure load-test-branch pattern (very strong idle loop indicator)
        is_pure_poll = (has_load and has_cmp_or_tst and not has_swi)
        if is_pure_poll and not has_store and not has_call:
            conf += 0.2
            reasons.append("pure_poll_pattern(+0.2)")
        elif is_pure_poll:
            conf += 0.1
            reasons.append("poll_pattern_impure(+0.1)")

        # Short loops are more likely to be idle loops
        if n <= 5:
            conf += 0.15
            reasons.append(f"short({n})(+0.15)")
        elif n <= 8:
            conf += 0.1
            reasons.append(f"medium({n})(+0.1)")

        # Loops with only 1 load and rest ALU+branch are very suspicious
        if load_count == 1 and alu_count >= 1 and n <= 6:
            conf += 0.1
            reasons.append("single_load_tight(+0.1)")

        # Near ROM entry (0x0800xxxx) — many known idle loops are here
        if branch_pc < ROM_BASE + 0x10000:
            conf += 0.1
            reasons.append("near_entry(+0.1)")

        if not io_reads and not ram_reads:
            # No traced memory target but still could be idle loop
            # (base register not resolved)
            if is_pure_poll and n <= 6:
                conf += 0.1
                reasons.append("unresolved_poll_tight(+0.1)")
            else:
                conf -= 0.15
                reasons.append("no_target_detected(-0.15)")

        if conf < 0.2:
            return None

        all_reads = io_reads + ram_reads
        conf = min(1.0, max(0.0, conf))
        return IdleCandidate(branch_pc, target_pc, n, "Thumb",
                             conf, all_reads, reasons, instrs)

    # ── ARM analysis ──

    def _scan_arm_precontext(self, target_pc: int, max_back: int = 16) -> Dict[int, int]:
        """Scan ARM instructions before the loop for register constants."""
        regs: Dict[int, int] = {}
        start_pc = max(ROM_BASE, target_pc - max_back * 4)
        pc = start_pc
        while pc < target_pc:
            o = self.off(pc)
            if o < 0 or o + 4 > self.size:
                pc += 4
                continue
            w = self.r32(o)
            pcl = ARMDec.pc_rel_load(w, pc)
            if pcl:
                rd, addr = pcl
                pool_off = self.off(addr)
                if 0 <= pool_off <= self.size - 4:
                    regs[rd] = self.r32(pool_off)
            # BL/BX clears context
            br = ARMDec.branch(w)
            if br and br[1]:
                regs.clear()
            pc += 4
        return regs

    def _analyze_arm_loop(self, branch_pc: int, target_pc: int) -> Optional[IdleCandidate]:
        n = (branch_pc - target_pc) // 4 + 1
        if not (self.MIN_LOOP_INSTRS <= n <= self.MAX_LOOP_INSTRS):
            return None

        pre_regs = self._scan_arm_precontext(target_pc)
        regs: Dict[int, int] = dict(pre_regs)
        has_store = has_bl = has_swi = False
        has_load = has_cmp = False
        io_reads: List[str] = []
        ram_reads: List[str] = []
        instrs: List[str] = []
        load_count = 0

        pc = target_pc
        while pc <= branch_pc:
            o = self.off(pc)
            if o < 0 or o + 4 > self.size:
                return None
            w = self.r32(o)
            instrs.append(f"  0x{pc:08X}: {w:08x}  {ARMDec.disasm(w, pc)}")

            if ARMDec.is_store(w): has_store = True
            if ARMDec.is_swi(w):   has_swi = True
            br = ARMDec.branch(w)
            if br and br[1]:  # BL
                has_bl = True

            # Check for CMP/TST
            if (w >> 26) & 3 == 0:
                op = (w >> 21) & 0xF
                if op in (8, 9, 10, 11):  # TST, TEQ, CMP, CMN
                    has_cmp = True

            pcl = ARMDec.pc_rel_load(w, pc)
            if pcl:
                rd, addr = pcl
                pool_off = self.off(addr)
                if 0 <= pool_off <= self.size - 4:
                    regs[rd] = self.r32(pool_off)

            base = ARMDec.get_load_base(w)
            if base is not None:
                has_load = True
                load_count += 1
                if base in regs:
                    baddr = regs[base]
                    if IO_BASE <= baddr < IO_END:
                        name = IO_POLL_REGS.get(baddr & ~1, f"IO:0x{baddr:08X}")
                        if name not in io_reads:
                            io_reads.append(name)
                    elif IWRAM_BASE <= baddr < IWRAM_END:
                        name = f"IWRAM:0x{baddr:08X}"
                        if name not in ram_reads:
                            ram_reads.append(name)
                    elif EWRAM_BASE <= baddr < EWRAM_END:
                        name = f"EWRAM:0x{baddr:08X}"
                        if name not in ram_reads:
                            ram_reads.append(name)

            pc += 4

        conf = 0.0
        reasons = []

        if has_store:
            conf -= 0.35
            reasons.append("store(-0.35)")
        if has_bl:
            conf -= 0.2
            reasons.append("bl(-0.2)")
        if has_swi:
            conf -= 0.3
            reasons.append("swi(-0.3)")
        if io_reads:
            conf += 0.5
            reasons.append(f"io_read(+0.5): {','.join(io_reads)}")
        if any(r in IO_STATUS_REGS for r in io_reads):
            conf += 0.3
            reasons.append("status_reg(+0.3)")
        if ram_reads and not io_reads:
            conf += 0.4
            reasons.append(f"ram_poll(+0.4): {','.join(ram_reads)}")

        is_pure_poll = (has_load and has_cmp and not has_swi)
        if is_pure_poll and not has_store and not has_bl:
            conf += 0.2
            reasons.append("pure_poll(+0.2)")
        elif is_pure_poll:
            conf += 0.1
            reasons.append("poll_impure(+0.1)")
        if n <= 5:
            conf += 0.15
            reasons.append(f"short({n})(+0.15)")
        elif n <= 8:
            conf += 0.1
            reasons.append(f"medium({n})(+0.1)")
        if load_count == 1 and n <= 5:
            conf += 0.1
            reasons.append("single_load_tight(+0.1)")
        if branch_pc < ROM_BASE + 0x10000:
            conf += 0.1
            reasons.append("near_entry(+0.1)")
        if not io_reads and not ram_reads:
            if is_pure_poll and n <= 6:
                conf += 0.1
                reasons.append("unresolved_poll_tight(+0.1)")
            else:
                conf -= 0.15
                reasons.append("no_target(-0.15)")
        if conf < 0.2:
            return None

        all_reads = io_reads + ram_reads
        conf = min(1.0, max(0.0, conf))
        return IdleCandidate(branch_pc, target_pc, n, "ARM",
                             conf, all_reads, reasons, instrs)

    # ── Scanners ──

    def scan_thumb(self):
        """Linear scan for Thumb backward conditional branches."""
        for off in range(0, self.size - 2, 2):
            hw = self.r16(off)
            pc = ROM_BASE + off

            cb = ThumbDec.cond_branch(hw)
            if cb:
                cond, rel = cb
                if rel < 0:  # backward
                    target = pc + rel
                    if target >= ROM_BASE:
                        c = self._analyze_thumb_loop(pc, target)
                        if c:
                            # Store all loop body PCs as alternates
                            c.alt_pcs = list(range(target, pc, 2))
                            self.candidates.append(c)

            # Detect unconditional self-branch: B . (0xe7fe)
            ub = ThumbDec.uncond_branch(hw)
            if ub is not None and ub == 0:
                # B . pattern: infinite loop — strong idle signal
                c = IdleCandidate(pc, pc, 1, "Thumb", 0.6, [],
                                  ["self_branch(+0.6)"],
                                  [f"  0x{pc:08X}: {hw:04x}       b  0x{pc:08x}"])
                if pc < ROM_BASE + 0x10000:
                    c.confidence = 0.7
                    c.reasons.append("near_entry(+0.1)")
                self.candidates.append(c)

    def scan_thumb_forward_poll(self):
        """Detect forward-branch poll functions called in tight outer loops.

        Pattern:
          PUSH {LR}
          LDR  Rn, [PC, #...] ; load address from pool
          LDRB/LDRH R0, [Rn, #imm]  ; read volatile
          CMP  R0, #imm
          Bcond forward      ; <-- idle_loop_target_pc
          [BL ...]
          ...
          POP {PC}

        This is common in Capcom/SNK engines: a function is called repeatedly
        from an outer loop. The function checks a flag and returns early (forward
        branch) if not set.
        """
        for off in range(0, self.size - 16, 2):
            hw = self.r16(off)
            pc = ROM_BASE + off

            # Look for PUSH {LR} as function start
            if hw not in (0xb500, 0xb510, 0xb530, 0xb570, 0xb5f0,
                          0xb580, 0xb590, 0xb5b0, 0xb5f8, 0xb501,
                          0xb502, 0xb504, 0xb508):
                # Quick filter: must be PUSH containing LR (bit 8 set)
                if (hw >> 9) != 0b1011010 or not (hw & 0x100):
                    continue

            # Scan forward for the pattern: LDR, LDRB/LDRH, CMP, Bcond_fwd
            regs: Dict[int, int] = {}
            has_load = False
            has_cmp = False
            io_reads: List[str] = []
            ram_reads: List[str] = []
            instrs: List[str] = []
            branch_pc = 0
            scan_pc = pc + 2
            max_scan = min(pc + 16, ROM_BASE + self.size)  # at most 8 instructions

            while scan_pc < max_scan:
                o = self.off(scan_pc)
                if o + 2 > self.size:
                    break
                instr = self.r16(o)
                instrs.append(f"  0x{scan_pc:08X}: {instr:04x}  {ThumbDec.disasm(instr, scan_pc)}")

                # Track LDR Rd, [PC, #imm]
                pcl = ThumbDec.pc_rel_load(instr, scan_pc)
                if pcl:
                    rd, addr = pcl
                    pool_off = self.off(addr)
                    if 0 <= pool_off <= self.size - 4:
                        regs[rd] = self.r32(pool_off)

                # Track loads
                rl = ThumbDec.reg_load(instr)
                if rl:
                    rd, rb, boff = rl
                    has_load = True
                    if rb in regs:
                        addr = (regs[rb] + boff) & 0xFFFFFFFF if boff >= 0 else regs[rb]
                        if IO_BASE <= addr < IO_END:
                            name = IO_POLL_REGS.get(addr & ~1, f"IO:0x{addr:08X}")
                            if name not in io_reads:
                                io_reads.append(name)
                        elif IWRAM_BASE <= addr < IWRAM_END:
                            name = f"IWRAM:0x{addr:08X}"
                            if name not in ram_reads:
                                ram_reads.append(name)
                        elif EWRAM_BASE <= addr < EWRAM_END:
                            name = f"EWRAM:0x{addr:08X}"
                            if name not in ram_reads:
                                ram_reads.append(name)

                # Check CMP
                if (instr >> 11) == 0b00101:  # CMP Rd, #imm8
                    has_cmp = True
                if (instr >> 10) == 0b010000:
                    op = (instr >> 6) & 0xF
                    if op in (8, 10):  # TST, CMP
                        has_cmp = True

                # Check forward conditional branch
                cb = ThumbDec.cond_branch(instr)
                if cb:
                    cond, rel = cb
                    if rel > 0 and has_load and has_cmp:
                        branch_pc = scan_pc
                        break

                # Abort on BX, unconditional branch, or another PUSH
                if ThumbDec.is_bx(instr) or ThumbDec.uncond_branch(instr) is not None:
                    break
                if (instr >> 9) == 0b1011010:  # another PUSH
                    break

                scan_pc += 2

            if not branch_pc or not has_load or not has_cmp:
                continue

            n = (branch_pc - pc) // 2 + 1
            if n > 10:
                continue

            conf = 0.0
            reasons = ["fwd_poll_func"]
            all_reads = io_reads + ram_reads

            if io_reads:
                conf += 0.5
                reasons.append(f"io_read(+0.5): {','.join(io_reads)}")
            if ram_reads and not io_reads:
                conf += 0.4
                reasons.append(f"ram_poll(+0.4): {','.join(ram_reads)}")
            if all_reads:
                conf += 0.2
                reasons.append("poll_with_target(+0.2)")
            else:
                conf += 0.1
                reasons.append("unresolved_fwd_poll(+0.1)")

            if branch_pc < ROM_BASE + 0x20000:
                conf += 0.1
                reasons.append("near_entry(+0.1)")
            if n <= 5:
                conf += 0.1
                reasons.append(f"short({n})(+0.1)")

            if conf < 0.2:
                continue

            conf = min(1.0, max(0.0, conf))
            c = IdleCandidate(branch_pc, pc, n, "Thumb",
                              conf, all_reads, reasons, instrs)
            self.candidates.append(c)

    def scan_thumb_fwd_to_bwd(self):
        """Detect forward-cond-branch → unconditional-backward-branch pattern.

        Pattern (common in Nintendo first-party engines):
          LOOP_TOP:
            ldr  Rn, [PC, #pool]    ; load address
            ldrh/ldrb R0, [Rn, #n]  ; read volatile
            [ALU ops]
            cmp  R0, #imm
            Bcond CONTINUE           ; forward conditional branch (= idle_loop_target_pc)
            B    EXIT                ; unconditional branch out
            [literal pool data]
          CONTINUE:
            B    LOOP_TOP            ; unconditional backward branch

        The idle_loop_target_pc is the forward conditional branch (Bcond).
        """
        for off in range(0, self.size - 2, 2):
            hw = self.r16(off)
            pc = ROM_BASE + off

            # Look for forward conditional branches
            cb = ThumbDec.cond_branch(hw)
            if not cb:
                continue
            cond, rel = cb
            if rel <= 0 or rel > 20:  # forward, but not too far
                continue

            fwd_target = pc + rel

            # Check: is the forward target an unconditional backward branch?
            fwd_off = self.off(fwd_target)
            if fwd_off + 2 > self.size:
                continue
            target_hw = self.r16(fwd_off)
            ub = ThumbDec.uncond_branch(target_hw)
            if ub is None or ub >= 0:  # must be backward
                continue

            loop_top = fwd_target + ub
            if loop_top < ROM_BASE:
                continue

            # The outer loop spans from loop_top to fwd_target (the uncond B)
            total_halfwords = (fwd_target - loop_top) // 2 + 1
            if total_halfwords < 3 or total_halfwords > 40:
                continue

            # Analyze the loop body from loop_top to fwd_target
            # Follow unconditional forward branches (skip literal pools)
            pre_regs = self._scan_thumb_precontext(loop_top)
            regs = dict(pre_regs)

            has_load = False
            has_cmp = False
            has_call = False
            has_store = False
            io_reads: List[str] = []
            ram_reads: List[str] = []
            instrs: List[str] = []
            load_count = 0

            scan_pc = loop_top
            while scan_pc <= fwd_target:
                o = self.off(scan_pc)
                if o + 2 > self.size:
                    break
                instr = self.r16(o)

                # Follow unconditional forward branches (skip literal pools)
                ub_rel = ThumbDec.uncond_branch(instr)
                if ub_rel is not None and ub_rel > 0:
                    new_pc = scan_pc + ub_rel
                    if new_pc <= fwd_target:
                        instrs.append(f"  0x{scan_pc:08X}: {instr:04x}  "
                                      f"{ThumbDec.disasm(instr, scan_pc)}  -> skip pool")
                        scan_pc = new_pc
                        continue

                mark = " <-- idle_pc" if scan_pc == pc else ""
                instrs.append(f"  0x{scan_pc:08X}: {instr:04x}  "
                              f"{ThumbDec.disasm(instr, scan_pc)}{mark}")

                # Track registers from PC-relative loads
                pcl = ThumbDec.pc_rel_load(instr, scan_pc)
                if pcl:
                    rd, addr = pcl
                    pool_off = self.off(addr)
                    if 0 <= pool_off <= self.size - 4:
                        regs[rd] = self.r32(pool_off)

                rl = ThumbDec.reg_load(instr)
                if rl:
                    rd, rb, boff = rl
                    has_load = True
                    load_count += 1
                    if rb in regs:
                        addr = (regs[rb] + boff) & 0xFFFFFFFF if boff >= 0 else regs[rb]
                        if IO_BASE <= addr < IO_END:
                            name = IO_POLL_REGS.get(addr & ~1, f"IO:0x{addr:08X}")
                            if name not in io_reads:
                                io_reads.append(name)
                        elif IWRAM_BASE <= addr < IWRAM_END:
                            name = f"IWRAM:0x{addr:08X}"
                            if name not in ram_reads:
                                ram_reads.append(name)
                        elif EWRAM_BASE <= addr < EWRAM_END:
                            name = f"EWRAM:0x{addr:08X}"
                            if name not in ram_reads:
                                ram_reads.append(name)

                if ThumbDec.is_cmp(instr) or ThumbDec.is_tst(instr):
                    has_cmp = True
                if ThumbDec.is_bl(instr):
                    has_call = True
                if ThumbDec.is_store(instr):
                    has_store = True

                scan_pc += 2

            if not has_load or not has_cmp:
                continue

            # Score
            n = total_halfwords
            conf = 0.0
            reasons = ["fwd_to_bwd"]
            all_reads = io_reads + ram_reads

            if has_store:
                conf -= 0.35
                reasons.append("store(-0.35)")
            if has_call:
                conf -= 0.2
                reasons.append("call(-0.2)")

            if io_reads:
                conf += 0.5
                reasons.append(f"io_read(+0.5): {','.join(io_reads)}")
            if any(r in IO_STATUS_REGS for r in io_reads):
                conf += 0.3
                reasons.append("status_reg(+0.3)")
            if ram_reads and not io_reads:
                conf += 0.4
                reasons.append(f"ram_poll(+0.4): {','.join(ram_reads)}")
            if all_reads and not has_store and not has_call:
                conf += 0.2
                reasons.append("clean_poll(+0.2)")
            elif all_reads:
                conf += 0.1
                reasons.append("poll_with_target(+0.1)")

            if pc < ROM_BASE + 0x10000:
                conf += 0.1
                reasons.append("near_entry(+0.1)")
            if n <= 8:
                conf += 0.15
                reasons.append(f"short({n})(+0.15)")
            elif n <= 12:
                conf += 0.05
                reasons.append(f"medium({n})(+0.05)")

            if not all_reads:
                if has_load and has_cmp and n <= 10:
                    conf += 0.05
                    reasons.append("unresolved_fwd_poll(+0.05)")
                else:
                    conf -= 0.15
                    reasons.append("no_target(-0.15)")

            if conf < 0.2:
                continue

            conf = min(1.0, max(0.0, conf))
            # The idle_loop_target_pc is the forward conditional branch
            c = IdleCandidate(pc, loop_top, n, "Thumb",
                              conf, all_reads, reasons, instrs)
            # Add all loop body PCs as alternates
            c.alt_pcs = list(range(loop_top, fwd_target + 2, 2))
            self.candidates.append(c)

    def scan_arm(self):
        """Linear scan for ARM backward conditional branches."""
        for off in range(0, self.size - 4, 4):
            w = self.r32(off)
            pc = ROM_BASE + off

            br = ARMDec.branch(w)
            if br:
                cond, is_link, rel = br
                if not is_link and rel < 0 and cond < 0xE:
                    target = pc + rel
                    if target >= ROM_BASE:
                        c = self._analyze_arm_loop(pc, target)
                        if c:
                            c.alt_pcs = list(range(target, pc, 4))
                            self.candidates.append(c)

    def find(self) -> List[IdleCandidate]:
        """Run full scan and return deduplicated, sorted candidates."""
        self.scan_thumb()
        tc = len(self.candidates)
        self.scan_thumb_forward_poll()
        fc = len(self.candidates) - tc
        self.scan_thumb_fwd_to_bwd()
        fbc = len(self.candidates) - tc - fc
        self.scan_arm()
        ac = len(self.candidates) - tc - fc - fbc

        # Deduplicate by PC (keep highest confidence, merge alt_pcs)
        best: Dict[int, IdleCandidate] = {}
        for c in self.candidates:
            if c.pc not in best or c.confidence > best[c.pc].confidence:
                if c.pc in best:
                    # Merge alt_pcs from lower-confidence duplicate
                    old_alts = set(best[c.pc].alt_pcs)
                    c.alt_pcs = list(set(c.alt_pcs) | old_alts)
                best[c.pc] = c
            else:
                # Merge alt_pcs into existing higher-confidence entry
                best[c.pc].alt_pcs = list(set(best[c.pc].alt_pcs) | set(c.alt_pcs))
        self.candidates = sorted(best.values(), key=lambda c: -c.confidence)
        return self.candidates

    # ── ROM header helpers ──

    def game_code(self) -> str:
        if self.size < 0xC0: return "????"
        return self.rom[0xAC:0xB0].decode('ascii', errors='replace')

    def game_title(self) -> str:
        if self.size < 0xAC: return "Unknown"
        return self.rom[0xA0:0xAC].decode('ascii', errors='replace').rstrip('\x00')


# ─── Analysis Runner ─────────────────────────────────────────────────────────

def analyze_rom(path: str, top_n: int = 10, verbose: bool = False,
                verify_pc: int = 0) -> List[IdleCandidate]:
    print(f"\n{'='*64}")
    print(f"  {os.path.basename(path)}")

    with open(path, 'rb') as f:
        data = f.read()

    finder = IdleLoopFinder(data)
    title = finder.game_title()
    code  = finder.game_code()
    print(f"  Title: {title}  Code: {code}  Size: {len(data)//1024}KB")

    candidates = finder.find()

    if verify_pc:
        found = [c for c in candidates if c.pc == verify_pc]
        if found:
            c = found[0]
            rank = candidates.index(c) + 1
            print(f"\n  ✓ VERIFIED 0x{verify_pc:08x} found at rank #{rank} "
                  f"(confidence {c.confidence:.0%})")
            for inst in c.instructions:
                print(f"    {inst}")
        else:
            print(f"\n  ✗ 0x{verify_pc:08x} NOT found by static analysis")
            # Show what's at that address anyway
            off = verify_pc - ROM_BASE
            if 0 <= off < len(data) - 2:
                hw = struct.unpack_from('<H', data, off)[0]
                print(f"    Instruction at that address: 0x{hw:04x}")
        return candidates

    if not candidates:
        print("  No candidates found.")
        return []

    show = min(top_n, len(candidates))
    print(f"\n  Top {show} of {len(candidates)} candidates:")
    print(f"  {'─'*58}")

    for i, c in enumerate(candidates[:show]):
        print(f"\n  #{i+1}  [{c.confidence:.0%}]  {c.mode} @ 0x{c.pc:08X}")
        print(f"      Loop: 0x{c.target:08X} → 0x{c.pc:08X} ({c.loop_size} instrs)")
        if c.io_reads:
            print(f"      IO:   {', '.join(c.io_reads)}")
        if verbose:
            print(f"      Why:  {'; '.join(c.reasons)}")
            for inst in c.instructions:
                print(f"      {inst}")

    # Output suggested gba_over.h entry
    if candidates:
        best = candidates[0]
        print(f"\n  Suggested gba_over.h entry:")
        print(f'    {{ "{code}", 0, 0x{best.pc:x}, 0, 0, 0 }},')

    return candidates


# ─── Dynamic Profiler Patch ──────────────────────────────────────────────────

DYNAMIC_PROFILER_PATCH = r"""
/* ═══════════════════════════════════════════════════════════════════════════
 * Idle Loop Dynamic Profiler for gpsp
 *
 * Compile with -DIDLE_LOOP_PROFILER to enable.
 * Add to cpu.h or a new header included from cpu.cc.
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifdef IDLE_LOOP_PROFILER

#include <string.h>
#include <stdio.h>

#define BPROF_SIZE  4096
#define BPROF_MASK  (BPROF_SIZE - 1)

static struct { u32 pc; u32 count; } bprof_table[BPROF_SIZE];
static u32 bprof_frames;

/* Call after every backward branch in the interpreter.
 * In the ARM loop (cpu.cc ~line 3108) after checking for branches:
 *     if (new_pc <= old_pc) bprof_record(old_pc);
 * In the Thumb loop (~line 3609) similarly.
 */
static inline void bprof_record(u32 pc) {
    u32 idx = ((pc >> 1) ^ (pc >> 13)) & BPROF_MASK;
    for (int i = 0; i < 8; i++) {
        u32 slot = (idx + i) & BPROF_MASK;
        if (bprof_table[slot].pc == pc) {
            bprof_table[slot].count++;
            return;
        }
        if (bprof_table[slot].pc == 0) {
            bprof_table[slot].pc = pc;
            bprof_table[slot].count = 1;
            return;
        }
    }
}

/* Call once per frame (e.g. in update_gba or vblank handler).
 * Dumps hot backward branches every 60 frames (~1 second). */
static inline void bprof_frame_tick(void) {
    if (++bprof_frames < 60)
        return;
    printf("=== Idle Loop Profiler: hot backward branches (60 frames) ===\n");
    for (int i = 0; i < BPROF_SIZE; i++) {
        /* Threshold: >10000 hits/sec is suspicious */
        if (bprof_table[i].count > 10000) {
            printf("  PC=0x%08x  hits=%u  (%.0f/frame)\n",
                   bprof_table[i].pc, bprof_table[i].count,
                   bprof_table[i].count / 60.0);
        }
    }
    printf("=============================================================\n");
    memset(bprof_table, 0, sizeof(bprof_table));
    bprof_frames = 0;
}

#else
#define bprof_record(pc)     ((void)0)
#define bprof_frame_tick()   ((void)0)
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * Integration points in cpu.cc interpreter:
 *
 * 1) ARM loop — after the idle_loop_target_pc check (~line 3112):
 *        if (reg[REG_PC] == idle_loop_target_pc ...) ...
 *    +   bprof_record(reg[REG_PC]);   // profile all PCs, filter later
 *
 *    Better: only record backward branches. Wrap the branch handlers:
 *    In ARM branch cases, when the new PC < old PC:
 *        bprof_record(old_pc);
 *
 * 2) Thumb loop — same pattern (~line 3609).
 *
 * 3) Frame boundary — in the main loop or update_gba():
 *        bprof_frame_tick();
 *
 * The output shows which PCs are hit most often. Cross-reference with
 * the static analysis tool to confirm idle loop candidates.
 * ═══════════════════════════════════════════════════════════════════════════ */
"""

# ─── Main ────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(
        description='GBA Idle Loop Finder — static + dynamic analysis tool')
    ap.add_argument('input', nargs='?',
                    help='ROM file, or directory with --batch')
    ap.add_argument('--batch', action='store_true',
                    help='Scan all .gba/.GBA files in directory')
    ap.add_argument('--top', type=int, default=5,
                    help='Show top N candidates (default 5)')
    ap.add_argument('--min-confidence', type=float, default=0.5,
                    help='Minimum confidence (default 0.5)')
    ap.add_argument('--verify', type=str, default=None,
                    help='Verify a known address, e.g. 0x80031d6')
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--dynamic-patch', action='store_true',
                    help='Print dynamic profiler C code')
    ap.add_argument('--csv', type=str, default=None,
                    help='Write batch results to CSV')

    args = ap.parse_args()

    if args.dynamic_patch:
        print(DYNAMIC_PROFILER_PATCH)
        return

    if not args.input:
        ap.print_help()
        return

    verify_pc = 0
    if args.verify:
        verify_pc = int(args.verify, 0)

    csv_rows = []

    if args.batch:
        roms = sorted(glob.glob(os.path.join(args.input, '**/*.gba'), recursive=True))
        roms += sorted(glob.glob(os.path.join(args.input, '**/*.GBA'), recursive=True))
        # Deduplicate (case-insensitive filesystems)
        seen = set()
        unique = []
        for r in roms:
            rp = os.path.realpath(r)
            if rp not in seen:
                seen.add(rp)
                unique.append(r)
        roms = unique
        print(f"Found {len(roms)} ROM files")

        for rom_path in roms:
            try:
                cs = analyze_rom(rom_path, top_n=args.top, verbose=args.verbose)
                good = [c for c in cs if c.confidence >= args.min_confidence]
                if good and args.csv:
                    with open(rom_path, 'rb') as f:
                        d = f.read()
                    fi = IdleLoopFinder(d)
                    b = good[0]
                    csv_rows.append(
                        f'"{fi.game_code()}","{fi.game_title()}",'
                        f'0x{b.pc:08x},{b.confidence:.2f},'
                        f'{b.mode},"{";".join(b.io_reads)}"')
            except Exception as e:
                print(f"  ERROR: {e}")
    else:
        if not os.path.isfile(args.input):
            print(f"Error: {args.input} not found")
            sys.exit(1)
        analyze_rom(args.input, top_n=args.top, verbose=args.verbose,
                    verify_pc=verify_pc)

    if args.csv and csv_rows:
        with open(args.csv, 'w') as f:
            f.write("game_code,title,idle_loop_pc,confidence,mode,io_reads\n")
            for row in csv_rows:
                f.write(row + "\n")
        print(f"\nCSV written to {args.csv}")


if __name__ == '__main__':
    main()
