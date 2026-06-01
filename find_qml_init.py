#!/usr/bin/env python3
"""Locate QQmlEngine::addImportPath / QQmlApplicationEngine::load call sites
in QingSnow2App and try to recover the QString argument literal where possible.

Approach:
  1. Find the PLT stub for the imported symbols by walking .rela.plt.
  2. Scan .text for `bl <plt_stub>` (aarch64 BL imm26).
  3. For each call site, look back a few instructions for the pattern that
     loads X0/X1 (QQmlEngine* / QString*) and try to follow it to a string.
     QString literals in Qt5 are usually built with QString::fromUtf8 or via
     a QArrayDataPointer pointing at .rodata. We try several heuristics.
"""
import sys, struct, re
from collections import defaultdict
from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
from capstone.arm64 import (ARM64_OP_IMM, ARM64_OP_REG, ARM64_OP_MEM,
                            ARM64_INS_BL, ARM64_INS_ADRP, ARM64_INS_ADD,
                            ARM64_INS_LDR, ARM64_INS_MOV, ARM64_INS_MOVZ,
                            ARM64_INS_MOVK)

BIN = "/Volumes/DATA/source/tmp/qingping/QingSnow2App"

TARGETS = {
    # mangled names of interest
    "_ZN10QQmlEngine13addImportPathERK7QString": "QQmlEngine::addImportPath",
    "_ZN21QQmlApplicationEngine4loadERK4QUrl": "QQmlApplicationEngine::load(QUrl)",
    "_ZN21QQmlApplicationEngine4loadERK7QString": "QQmlApplicationEngine::load(QString)",
    "_ZN21QQmlApplicationEngineC1EP7QObject": "QQmlApplicationEngine::QQmlApplicationEngine",
    "_ZN21QQmlApplicationEngineC2EP7QObject": "QQmlApplicationEngine::QQmlApplicationEngine",
    "_ZN21QQmlApplicationEngineC1ERK4QUrlP7QObject": "QQmlApplicationEngine(url)",
    "_ZN21QQmlApplicationEngineC2ERK4QUrlP7QObject": "QQmlApplicationEngine(url)",
    "_ZN11QQmlContext18setContextPropertyERK7QStringP7QObject":
        "QQmlContext::setContextProperty(QObject*)",
    "_ZN11QQmlContext18setContextPropertyERK7QStringRK8QVariant":
        "QQmlContext::setContextProperty(QVariant)",
    "_ZN7QString10fromUtf8_2EPKcx": "QString::fromUtf8(ptr, len)",   # qt5 inline
    "_ZN7QString8fromUtf8EPKcx": "QString::fromUtf8(ptr, len)",
    "_ZN7QString8fromUtf8EPKci": "QString::fromUtf8(ptr, int)",
    "_ZN7QString9fromLatin1EPKci": "QString::fromLatin1",
    "_ZN7QString9fromAsciiEPKci": "QString::fromAscii",
}

def main():
    with open(BIN, "rb") as f:
        elf = ELFFile(f)
        # Load .text and .rodata into memory
        text_sec  = elf.get_section_by_name(".text")
        plt_sec   = elf.get_section_by_name(".plt")
        rodata    = elf.get_section_by_name(".rodata")
        dynsym    = elf.get_section_by_name(".dynsym")
        relaplt   = elf.get_section_by_name(".rela.plt")

        text_base = text_sec["sh_addr"]; text_bytes = text_sec.data()
        plt_base  = plt_sec["sh_addr"];  plt_bytes  = plt_sec.data()
        rodata_base = rodata["sh_addr"]; rodata_bytes = rodata.data()

        # Map: symbol_name -> .got.plt entry, then derive PLT stub for it.
        # Easier: walk rela.plt entries in order; each maps to PLT slot i.
        # PLT layout in aarch64: header = 32 bytes, then 16 bytes per stub.
        # Slot i starts at plt_base + 32 + i*16.
        sym_to_plt = {}
        sym_table = dynsym
        for i, rel in enumerate(relaplt.iter_relocations()):
            sym = sym_table.get_symbol(rel["r_info_sym"])
            sym_to_plt[sym.name] = plt_base + 32 + i * 16

        # Also map every relocation in .rela.dyn that points to a symbol we
        # might need.

    # Build reverse map: plt_addr -> friendly name
    target_plts = {}
    for mangled, friendly in TARGETS.items():
        if mangled in sym_to_plt:
            target_plts[sym_to_plt[mangled]] = (mangled, friendly)
            print(f"  PLT 0x{sym_to_plt[mangled]:x}  {friendly}  ({mangled})")
        else:
            print(f"  -- not imported: {mangled}")

    if not target_plts:
        return

    md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN); md.detail = True

    # Cache decoded text for full sweep so we can look backwards.
    # Decoding 2MB of code is doable but we only need call sites.
    print("\n[scanning .text for BL to target PLTs]")
    callsites = defaultdict(list)
    insns_at = {}            # vaddr -> insn (lazy fill)

    # Pre-scan: find every BL <imm> instruction.
    # BL encoding: bits[31..26] = 100101, imm26 in low bits, sign-extended * 4.
    for i in range(0, len(text_bytes), 4):
        word = struct.unpack_from("<I", text_bytes, i)[0]
        if (word >> 26) == 0b100101:           # BL
            imm26 = word & 0x03FFFFFF
            if imm26 & 0x02000000:             # sign-extend
                imm26 |= ~0x03FFFFFF & 0xFFFFFFFF
                imm26 = imm26 - 0x100000000
            tgt = text_base + i + imm26 * 4
            if tgt in target_plts:
                callsites[tgt].append(text_base + i)

    for plt, sites in callsites.items():
        mangled, friendly = target_plts[plt]
        print(f"\n=== {friendly}  ({len(sites)} call sites) ===")
        for site in sites:
            print(f"  call at 0x{site:x}")
            # Disassemble backwards up to 40 instructions to find x0/x1 setup.
            start = max(text_base, site - 40 * 4)
            blob = text_bytes[start - text_base : site - text_base + 4]
            insns = list(md.disasm(blob, start))
            # Track x0..x2 reaching the call. Very rough taint.
            regs = {}
            for ins in insns:
                mid = ins.id
                ops = ins.operands
                if mid == ARM64_INS_ADRP and len(ops) == 2:
                    rd  = ins.reg_name(ops[0].reg)
                    imm = ops[1].imm
                    regs[rd] = ("adrp", imm)
                elif mid == ARM64_INS_ADD and len(ops) == 3:
                    rd  = ins.reg_name(ops[0].reg)
                    rn  = ins.reg_name(ops[1].reg)
                    if ops[2].type == ARM64_OP_IMM and rn in regs and regs[rn][0] == "adrp":
                        regs[rd] = ("addr", regs[rn][1] + ops[2].imm)
                    elif ops[2].type == ARM64_OP_IMM and rn in regs and regs[rn][0] == "addr":
                        regs[rd] = ("addr", regs[rn][1] + ops[2].imm)
                    else:
                        regs.pop(rd, None)
                elif mid == ARM64_INS_LDR and len(ops) == 2:
                    rd  = ins.reg_name(ops[0].reg)
                    if ops[1].type == ARM64_OP_MEM:
                        base = ins.reg_name(ops[1].mem.base)
                        bv = regs.get(base)
                        if bv and bv[0] in ("adrp","addr"):
                            ptr = bv[1] + ops[1].mem.disp
                            regs[rd] = ("load", ptr)
                        else:
                            regs.pop(rd, None)
                    elif ops[1].type == ARM64_OP_IMM:
                        regs[rd] = ("imm", ops[1].imm)
                    else:
                        regs.pop(rd, None)
                elif mid in (ARM64_INS_MOV, ARM64_INS_MOVZ) and len(ops) == 2:
                    rd  = ins.reg_name(ops[0].reg)
                    if ops[1].type == ARM64_OP_IMM:
                        regs[rd] = ("imm", ops[1].imm)
                    elif ops[1].type == ARM64_OP_REG:
                        rn = ins.reg_name(ops[1].reg)
                        regs[rd] = regs.get(rn)
                    else:
                        regs.pop(rd, None)
                else:
                    # Conservative: any insn touching a reg invalidates it.
                    if hasattr(ins, "regs_access"):
                        try:
                            _, written = ins.regs_access()
                            for r in written:
                                regs.pop(ins.reg_name(r), None)
                        except Exception:
                            pass

            x0 = regs.get("x0"); x1 = regs.get("x1"); x2 = regs.get("x2")
            print(f"     x0={x0}  x1={x1}  x2={x2}")

            # Try to resolve a string at the recovered address.
            for label, val in (("x0", x0), ("x1", x1), ("x2", x2)):
                if not val: continue
                kind, addr = val
                if kind not in ("addr", "load"): continue
                if not (rodata_base <= addr < rodata_base + len(rodata_bytes)):
                    continue
                off = addr - rodata_base
                # Try ASCII C-string
                end = rodata_bytes.find(b"\x00", off)
                s = rodata_bytes[off:end] if end != -1 else b""
                if s and all(0x20 <= b < 0x7f for b in s) and len(s) < 200:
                    print(f"     -> {label} -> rodata 0x{addr:x} = {s!r}")
                else:
                    # Maybe it's a QString static data block: header then UTF-16 data
                    # Header is 24 bytes (QArrayData), followed by UTF-16 bytes.
                    raw = rodata_bytes[off:off+24]
                    if len(raw) == 24:
                        size = struct.unpack_from("<i", raw, 4)[0]
                        offset = struct.unpack_from("<q", raw, 16)[0]
                        if 0 < size < 1024 and 0 < offset < 4096:
                            data_off = off + offset
                            data = rodata_bytes[data_off : data_off + size*2]
                            try:
                                txt = data.decode("utf-16-le")
                                print(f"     -> {label} -> QString@0x{addr:x} = {txt!r}")
                            except Exception:
                                pass

if __name__ == "__main__":
    main()
