#!/usr/bin/env python3
"""Walk all qrc trees registered by the binary via qRegisterResourceData.

Strategy: locate every call site of `_Z21qRegisterResourceDataiPKhS0_S0_`,
recover its 4 args (version:int, tree:ptr, names:ptr, data:ptr) by tracing
adrp+add for x1/x2/x3 in the preceding few instructions, then parse the qrc
tree following the standard Qt format.

Outputs a manifest of every qrc:/ path the binary owns, plus dumps QML/JS
files to ./qrc_out/<original_path>.
"""
import os, sys, struct
from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

BIN = "/Volumes/DATA/source/tmp/qingping/QingSnow2App"
OUT = "/Volumes/DATA/source/tmp/qingping/qrc_out"
os.makedirs(OUT, exist_ok=True)

with open(BIN, "rb") as f:
    elf = ELFFile(f)
    sections = {}
    for s in elf.iter_sections():
        if s["sh_size"]:
            sections[s.name] = (s["sh_addr"], s.data())
    text_base, text_bytes = sections[".text"]
    dynsym = elf.get_section_by_name(".dynsym")
    relaplt = elf.get_section_by_name(".rela.plt")
    plt = elf.get_section_by_name(".plt")
    plt_base = plt["sh_addr"]

    target_sym = "_Z21qRegisterResourceDataiPKhS0_S0_"
    target_plt = None
    for i, rel in enumerate(relaplt.iter_relocations()):
        s = dynsym.get_symbol(rel["r_info_sym"])
        if s.name == target_sym:
            target_plt = plt_base + 32 + i * 16
            break

print(f"qRegisterResourceData PLT: {target_plt:#x}")

def addr_to_offset(addr):
    for name, (base, data) in sections.items():
        if base <= addr < base + len(data):
            return name, addr - base
    return None, None

def read_at(addr, length):
    name, off = addr_to_offset(addr)
    if name is None: return None
    return sections[name][1][off : off + length]

md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN); md.detail = True

# Find every BL <target_plt> in .text
sites = []
for i in range(0, len(text_bytes), 4):
    word = struct.unpack_from("<I", text_bytes, i)[0]
    if (word >> 26) == 0b100101:
        imm26 = word & 0x03FFFFFF
        if imm26 & 0x02000000:
            imm26 = imm26 - 0x04000000
        tgt = text_base + i + imm26 * 4
        if tgt == target_plt:
            sites.append(text_base + i)
print(f"call sites: {len(sites)}")

def trace_args(call_addr):
    """Walk back up to 24 insns, recover x0..x3 const values where possible."""
    start = max(text_base, call_addr - 24 * 4)
    blob = text_bytes[start - text_base : call_addr - text_base]
    insns = list(md.disasm(blob, start))
    regs = {}
    for ins in insns:
        m = ins.mnemonic
        ops = ins.operands
        if m == "adrp":
            regs[ins.reg_name(ops[0].reg)] = ("adrp", ops[1].imm)
        elif m == "add" and len(ops) == 3 and ops[2].type == 2:
            rd = ins.reg_name(ops[0].reg); rn = ins.reg_name(ops[1].reg)
            v = regs.get(rn)
            if v and v[0] in ("adrp","addr"):
                regs[rd] = ("addr", v[1] + ops[2].imm)
            else:
                regs.pop(rd, None)
        elif m in ("mov", "movz") and len(ops) == 2:
            rd = ins.reg_name(ops[0].reg)
            # normalise w-reg to x-reg
            if rd.startswith("w"):
                rd = "x" + rd[1:]
            if ops[1].type == 2:
                regs[rd] = ("imm", ops[1].imm)
            elif ops[1].type == 1:
                rs = ins.reg_name(ops[1].reg)
                if rs.startswith("w"): rs = "x" + rs[1:]
                regs[rd] = regs.get(rs)
            else:
                regs.pop(rd, None)
        else:
            try:
                _, w = ins.regs_access()
                for r in w:
                    regs.pop(ins.reg_name(r), None)
            except Exception:
                pass
    return regs.get("x0"), regs.get("x1"), regs.get("x2"), regs.get("x3")

def u16(b, o): return struct.unpack_from(">H", b, o)[0]
def u32(b, o): return struct.unpack_from(">I", b, o)[0]
def u64(b, o): return struct.unpack_from(">Q", b, o)[0]

def parse_qrc(version, tree_addr, names_addr, data_addr):
    # Tree node sizes:
    #   v1: 14 bytes
    #   v2: 22 bytes (adds 8-byte mtime)
    #   v3: 22 bytes also (Qt 5.13+ same layout, with hash collisions allowed)
    node_size = 14 if version < 2 else 22
    # Resolve the three blobs as raw byte regions.
    def grab_blob(addr, hint_size=None):
        name, off = addr_to_offset(addr)
        if name is None: return None, None
        # Take generously, parse on the fly.
        data = sections[name][1][off:]
        return data, name
    tree, _ = grab_blob(tree_addr)
    names, _ = grab_blob(names_addr)
    payload, _ = grab_blob(data_addr)
    if tree is None or names is None or payload is None:
        return []

    def get_name(name_off):
        if name_off + 6 > len(names): return ""
        nlen = u16(names, name_off)
        # 4 bytes hash, then UTF-16BE name
        s = names[name_off + 6 : name_off + 6 + nlen*2]
        try:
            return s.decode("utf-16-be", errors="replace")
        except Exception:
            return ""

    def read_node(i):
        off = i * node_size
        if off + node_size > len(tree): return None
        name_off = u32(tree, off + 0)
        flags    = u16(tree, off + 4)
        if flags & 0x2:           # directory
            child_count = u32(tree, off + 6)
            child_off   = u32(tree, off + 10)
            return ("dir", name_off, child_count, child_off)
        else:                     # file
            country  = u16(tree, off + 6)
            language = u16(tree, off + 8)
            data_off = u32(tree, off + 10)
            return ("file", name_off, flags, data_off)

    files = []
    def walk(node_idx, path):
        n = read_node(node_idx)
        if not n: return
        if n[0] == "dir":
            name = get_name(n[1])
            new_path = path if name == "" else (path + "/" + name).replace("//", "/")
            base_child = n[3]
            for k in range(n[2]):
                walk(base_child + k, new_path)
        else:
            name = get_name(n[1])
            full = (path + "/" + name).replace("//", "/")
            flags = n[2]
            data_off = n[3]
            if data_off + 4 > len(payload): return
            data_len = u32(payload, data_off)
            blob = payload[data_off + 4 : data_off + 4 + data_len]
            # flag 0x1 = compressed (zlib)
            if flags & 0x1:
                import zlib
                try:
                    blob = zlib.decompress(blob[4:])  # 4-byte uncompressed-size prefix
                except Exception:
                    pass
            files.append((full, blob))
    walk(0, "")
    return files

for site in sites:
    x0, x1, x2, x3 = trace_args(site)
    print(f"\ncall {site:#x}  args: x0={x0} x1={x1} x2={x2} x3={x3}")
    if not (x0 and x0[0] == "imm" and x1 and x2 and x3):
        print("  (could not recover all args)")
        continue
    version = x0[1]
    tree_addr = x1[1]; names_addr = x2[1]; data_addr = x3[1]
    print(f"  version={version}  tree={tree_addr:#x}  names={names_addr:#x}  data={data_addr:#x}")
    files = parse_qrc(version, tree_addr, names_addr, data_addr)
    print(f"  files: {len(files)}")
    for full, blob in files:
        # Strip leading slashes
        rel = full.lstrip("/")
        if not rel: continue
        out_path = os.path.join(OUT, rel)
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        with open(out_path, "wb") as g:
            g.write(blob)
print(f"\nDone. Output in {OUT}")
