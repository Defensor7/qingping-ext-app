#!/usr/bin/env python3
"""Dump Application::registerQmlRootObjects and registerQmlTypes.

For setContextProperty calls we want pairs (name, object-class).
For qmlRegister*<T> calls we want (uri, major, minor, qmlName).

Strategy:
- Disassemble the whole function (follow `ret`).
- Track x0..x4 with adrp+add and ldr-from-pool / mov immediate.
- When we hit a BL into the PLT of one of our interesting targets, print
  the recovered registers along with any string they resolve to.
"""
import struct
from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

BIN = "/Volumes/DATA/source/tmp/qingping/QingSnow2App"

# Address of each function (entry).
FUNCS = {
    "Application::registerQmlRootObjects": 0x522414,
    "Application::registerQmlTypes":       0x525040,
}

# Friendly names for PLT targets we care about.
HINTS = {
    "_ZN11QQmlContext18setContextPropertyERK7QStringP7QObject":
        "ctx.setContextProperty(name, QObject*)",
    "_ZN11QQmlContext18setContextPropertyERK7QStringRK8QVariant":
        "ctx.setContextProperty(name, QVariant)",
    "_ZN10QQmlEngine11rootContextEv": "engine.rootContext()",
    "_ZN11QQmlPrivate11qmlregisterENS_16RegistrationTypeEPv":
        "QQmlPrivate::qmlregister(type, ptr)",
    "_ZN15QMetaTypeIdQObjectIP7QObjectLi8EE14qt_metatype_idEv":
        "qt_metatype_id(QObject*)",
    "_ZN7QStringC2EPKc": "QString::QString(const char*)",
    "_ZN7QStringC1EPKc": "QString::QString(const char*)",
    "_ZN7QStringC2EPKcx": "QString::QString(const char*, n)",
    "_ZN7QString8fromUtf8EPKci": "QString::fromUtf8",
    "_ZN7QStringD2Ev": "QString::~QString",
    "_ZN7QStringD1Ev": "QString::~QString",
    "_ZN4QUrlC1ERK7QString11ParsingMode": "QUrl::QUrl(QString)",
    "_Znwm": "operator new",
    "_ZdlPv": "operator delete",
}

with open(BIN, "rb") as f:
    elf = ELFFile(f)
    secs = {s.name: (s["sh_addr"], s.data()) for s in elf.iter_sections() if s["sh_size"]}
    text_base, text_bytes = secs[".text"]
    rodata_base, rodata_bytes = secs[".rodata"]
    dynsym = elf.get_section_by_name(".dynsym")
    relaplt = elf.get_section_by_name(".rela.plt")
    plt = elf.get_section_by_name(".plt")
    plt_base = plt["sh_addr"]
    symtab = elf.get_section_by_name(".symtab")

    # Local function names (for BL targets inside .text)
    addr_to_func = {}
    for s in symtab.iter_symbols():
        if s["st_info"]["type"] == "STT_FUNC" and s["st_size"] > 0:
            addr_to_func[s["st_value"]] = s.name

    # PLT slot -> imported symbol name
    plt_to_sym = {}
    for i, rel in enumerate(relaplt.iter_relocations()):
        sym = dynsym.get_symbol(rel["r_info_sym"])
        plt_to_sym[plt_base + 32 + i * 16] = sym.name

# Quick scan to map .rodata addr -> any string we can recover.
def read_cstr(addr):
    if not (rodata_base <= addr < rodata_base + len(rodata_bytes)):
        return None
    o = addr - rodata_base
    e = rodata_bytes.find(b"\x00", o)
    s = rodata_bytes[o:e] if e != -1 else b""
    if s and all(0x20 <= b < 0x7f for b in s) and len(s) < 200:
        return s.decode("latin1")
    return None

def read_qstring_static(addr):
    """Qt5 QArrayData header (24B) + UTF-16 data."""
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

def resolve(addr):
    s = read_qstring_static(addr)
    if s is not None: return f'u"{s}"'
    s = read_cstr(addr)
    if s is not None: return f'c"{s}"'
    return None

md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN); md.detail = True

def dump_function(name, entry):
    print(f"\n{'='*70}\n=== {name}  @ {entry:#x} ===\n{'='*70}")
    # Find end via .symtab size.
    size = None
    for a, n in addr_to_func.items():
        if a == entry:
            # we have name + addr but not size here; look up in symtab
            with open(BIN, "rb") as f:
                e2 = ELFFile(f)
                st = e2.get_section_by_name(".symtab")
                for s in st.iter_symbols():
                    if s["st_value"] == entry:
                        size = s["st_size"]
                        break
    if size is None:
        size = 0x1000   # fallback
    blob = text_bytes[entry - text_base : entry - text_base + size]
    insns = list(md.disasm(blob, entry))
    print(f"({len(insns)} insns, size={size:#x})")

    regs = {}
    last_calls = []   # buffer last few resolved-arg states for context
    for ins in insns:
        m = ins.mnemonic
        ops = ins.operands

        # ---- tracking ----
        def setreg(name, val): regs[name] = val
        def killreg(name): regs.pop(name, None)

        if m == "adrp":
            rd = ins.reg_name(ops[0].reg)
            setreg(rd, ("adrp", ops[1].imm))
        elif m == "add" and len(ops) == 3 and ops[2].type == 2:
            rd = ins.reg_name(ops[0].reg)
            rn = ins.reg_name(ops[1].reg)
            v = regs.get(rn)
            if v and v[0] in ("adrp", "addr"):
                setreg(rd, ("addr", v[1] + ops[2].imm))
            else:
                killreg(rd)
        elif m == "ldr" and len(ops) == 2 and ops[1].type == 3:
            rd = ins.reg_name(ops[0].reg)
            base = ins.reg_name(ops[1].mem.base)
            disp = ops[1].mem.disp
            v = regs.get(base)
            if v and v[0] in ("adrp", "addr"):
                a = v[1] + disp
                # Sometimes this is a literal pointer table; we still note the source addr.
                setreg(rd, ("loaded_from", a))
            else:
                killreg(rd)
        elif m in ("mov", "movz") and len(ops) >= 2:
            rd = ins.reg_name(ops[0].reg)
            if rd.startswith("w"): rd = "x" + rd[1:]
            if ops[1].type == 2:
                setreg(rd, ("imm", ops[1].imm))
            elif ops[1].type == 1:
                rs = ins.reg_name(ops[1].reg)
                if rs.startswith("w"): rs = "x" + rs[1:]
                setreg(rd, regs.get(rs))
            else:
                killreg(rd)
        elif m == "bl":
            # snapshot regs at call site
            tgt = ops[0].imm
            tgt_name = (HINTS.get(plt_to_sym.get(tgt), plt_to_sym.get(tgt))
                        or addr_to_func.get(tgt)
                        or f"sub_{tgt:x}")
            snap = {r: regs.get(r) for r in ("x0","x1","x2","x3","x4")}
            interesting = tgt_name and ("setContextProperty" in tgt_name
                                        or "qmlregister" in tgt_name
                                        or "QString" in tgt_name and "fromUtf8" in tgt_name
                                        or "operator new" in tgt_name
                                        or "fromUtf8" in tgt_name
                                        or "QUrl" in tgt_name)
            tag = "  >>>" if interesting else "     "
            sumry = []
            for r, v in snap.items():
                if not v: continue
                kind, val = v
                if kind in ("addr", "loaded_from"):
                    s = resolve(val)
                    sumry.append(f"{r}={val:#x}{(' '+s) if s else ''}")
                elif kind == "imm":
                    sumry.append(f"{r}=#{val}")
            print(f"{tag} {ins.address:08x} bl {tgt:#x}  -> {tgt_name}")
            if sumry:
                print(f"        " + "  ".join(sumry))
            # After BL, x0 typically holds return; x0..x18 are caller-clobbered.
            for r in ("x0","x1","x2","x3","x4","x5","x6","x7","x8","x9","x10",
                      "x11","x12","x13","x14","x15","x16","x17","x18"):
                killreg(r)
        elif m == "ret":
            break
        else:
            try:
                _, w = ins.regs_access()
                for r in w:
                    killreg(ins.reg_name(r))
            except Exception:
                pass

for name, entry in FUNCS.items():
    dump_function(name, entry)
