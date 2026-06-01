#!/usr/bin/env python3
"""Dump the disassembly window that contains the QML engine setup.
Annotate every BL with a symbol name (PLT -> .rela.plt -> dynsym).
Resolve adrp+add/ldr -> string literal where possible.
"""
import struct
from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

BIN = "/Volumes/DATA/source/tmp/qingping/QingSnow2App"
START = 0x5251c0
END   = 0x5253c0

# Some demangled aliases for common Qt5 mangled names we expect to hit.
DEMANGLE_HINTS = {
    "_ZN10QQmlEngine13addImportPathERK7QString": "QQmlEngine::addImportPath",
    "_ZN21QQmlApplicationEngineC1EP7QObject":    "QQmlApplicationEngine::ctor",
    "_ZN21QQmlApplicationEngine4loadERK4QUrl":   "QQmlApplicationEngine::load(QUrl)",
    "_ZN11QQmlContext18setContextPropertyERK7QStringP7QObject": "ctx.setContextProperty(QObject)",
    "_ZN11QQmlContext18setContextPropertyERK7QStringRK8QVariant": "ctx.setContextProperty(QVariant)",
    "_ZN10QQmlEngine11rootContextEv": "engine.rootContext()",
    "_ZN4QUrlC1ERK7QString11ParsingMode": "QUrl::QUrl(QString)",
    "_ZN4QUrlC2ERK7QString11ParsingMode": "QUrl::QUrl(QString)",
    "_ZN4QUrlD1Ev": "QUrl::~QUrl",
    "_ZN4QUrlD2Ev": "QUrl::~QUrl",
    "_ZN7QStringD1Ev": "QString::~QString",
    "_ZN7QStringD2Ev": "QString::~QString",
    "_ZN7QString16fromAscii_helperEPKci": "QString::fromAscii_helper",
    "_ZN7QString21fromUtf8_helperEPKcx":  "QString::fromUtf8_helper",
    "_ZN7QString15fromUtf16HelperEPKtx":  "QString::fromUtf16Helper",
    "_ZN9QtPrivate18qStringLiteralBaseEPKtxN13QArrayDataPtr18LiteralStorageTypeE": "qStringLiteralBase",
}

with open(BIN, "rb") as f:
    elf = ELFFile(f)
    text   = elf.get_section_by_name(".text")
    rodata = elf.get_section_by_name(".rodata")
    plt    = elf.get_section_by_name(".plt")
    dynsym = elf.get_section_by_name(".dynsym")
    relaplt= elf.get_section_by_name(".rela.plt")
    symtab = elf.get_section_by_name(".symtab")

    text_base = text["sh_addr"]; text_bytes = text.data()
    rodata_base = rodata["sh_addr"]; rodata_bytes = rodata.data()
    plt_base  = plt["sh_addr"]

    # plt slot i -> symbol name
    plt_to_sym = {}
    for i, rel in enumerate(relaplt.iter_relocations()):
        s = dynsym.get_symbol(rel["r_info_sym"])
        plt_to_sym[plt_base + 32 + i*16] = s.name

    # Local function map (we're not stripped, so .symtab gives us names).
    addr_to_func = {}
    if symtab:
        for s in symtab.iter_symbols():
            if s["st_info"]["type"] == "STT_FUNC" and s["st_size"] > 0:
                addr_to_func[s["st_value"]] = s.name

def read_cstr(addr):
    if not (rodata_base <= addr < rodata_base + len(rodata_bytes)):
        return None
    o = addr - rodata_base
    e = rodata_bytes.find(b"\x00", o)
    s = rodata_bytes[o:e] if e != -1 else b""
    if s and all(0x20 <= b < 0x7f or b in (9,10) for b in s) and len(s) < 200:
        return s.decode("latin1", errors="replace")
    return None

def read_qstring_static(addr):
    """Qt5 QArrayData header (24B) followed by UTF-16 data."""
    if not (rodata_base <= addr < rodata_base + len(rodata_bytes)):
        return None
    o = addr - rodata_base
    raw = rodata_bytes[o:o+24]
    if len(raw) < 24: return None
    ref, size, alloc, flags, offset = struct.unpack_from("<iiIIQ", raw, 0)
    if not (0 < size < 1024 and 0 < offset < 4096):
        return None
    data = rodata_bytes[o+offset : o+offset + size*2]
    try:
        return data.decode("utf-16-le")
    except Exception:
        return None

def annotate_addr(addr):
    """Return any string we can recover at addr."""
    s = read_qstring_static(addr)
    if s is not None: return f'u"{s}"'
    s = read_cstr(addr)
    if s is not None: return f'c"{s}"'
    return None

md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN); md.detail = True

off = START - text_base
blob = text_bytes[off : END - text_base]
regs = {}   # rough const tracker

print(f"# {START:#x} .. {END:#x}")
for ins in md.disasm(blob, START):
    line = f"  {ins.address:08x}: {ins.mnemonic:6s} {ins.op_str}"
    note = ""

    if ins.mnemonic == "adrp":
        rd = ins.operands[0].reg; imm = ins.operands[1].imm
        regs[rd] = ("adrp", imm)
    elif ins.mnemonic == "add":
        ops = ins.operands
        if len(ops) == 3 and ops[2].type == 2:    # IMM
            rd = ops[0].reg; rn = ops[1].reg
            v = regs.get(rn)
            if v and v[0] in ("adrp","addr"):
                a = v[1] + ops[2].imm
                regs[rd] = ("addr", a)
                s = annotate_addr(a)
                if s: note = f"   ; -> {a:#x} {s}"
            else:
                regs.pop(rd, None)
        else:
            regs.pop(ins.operands[0].reg, None)
    elif ins.mnemonic == "ldr":
        ops = ins.operands
        if len(ops) == 2 and ops[1].type == 3:    # MEM
            rd = ops[0].reg; base = ops[1].mem.base; disp = ops[1].mem.disp
            v = regs.get(base)
            if v and v[0] in ("adrp","addr"):
                a = v[1] + disp
                regs[rd] = ("loaded_from", a)
                # Many string literals live behind a single ldr from .data.rel.ro
                s = annotate_addr(a)
                if s: note = f"   ; ldr <- {a:#x} {s}"
                else: note = f"   ; ldr <- {a:#x}"
            else:
                regs.pop(rd, None)
        else:
            regs.pop(ins.operands[0].reg, None)
    elif ins.mnemonic == "bl":
        tgt = ins.operands[0].imm
        if tgt in plt_to_sym:
            sym = plt_to_sym[tgt]
            note = f"   ; -> {DEMANGLE_HINTS.get(sym, sym)}"
        elif tgt in addr_to_func:
            note = f"   ; -> {addr_to_func[tgt]}"
        else:
            note = f"   ; -> sub_{tgt:x}"
    elif ins.mnemonic in ("mov","movz"):
        if len(ins.operands) >= 2 and ins.operands[1].type == 2:
            regs[ins.operands[0].reg] = ("imm", ins.operands[1].imm)

    print(line + note)
