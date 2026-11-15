#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Count the instructions of IGC (Gen/Xe) kernel assembly dumps, per basic block.

Dump first (nothing runs on the GPU):
  NEO_CACHE_PERSISTENT=0 SYCL_CACHE_PERSISTENT=0 ARCH_COMPILE_ONLY=1 IGC_ShaderDumpEnable=1 IGC_DumpToCustomDir=<dir>/ bin/gemm_stages
then:
  scripts/igc_asm_count.py <dir> [substring of the kernel name ...]
For every matching *.asm it prints the header facts (numGRF, instruction count, spill size) and, for the basic block with the
most instructions (the unrolled loop body) and for the whole kernel, the instruction counts by class.
"""
import collections, glob, os, re, sys

CLASSES = [
    ("slm_gather", r"^load\.slm\.d32\.a32$|^load\.slm\.d(8|16)u32\.a32$"),
    ("slm_block_load", r"^load\.slm\.d(32|64)x\d+t"),
    ("slm_store", r"^store\.slm"),
    ("usm_gather", r"^load\.ugm\.d32\.a64$|^load\.ugm\.d(8|16)u32\.a64$"),
    ("usm_block_load", r"^load\.ugm\.d(32|64)x\d+t"),
    ("usm_store", r"^store\.ugm"),
    ("scratch_fill", r"^load\.(ugm|tgm)\..*scratch|^fill"),
    ("scratch_spill", r"^store\.(ugm|tgm)\..*scratch|^spill"),
    ("dpas", r"^dpas"),
    ("shift", r"^(shl|shr|asr|rol|ror)$"),
    ("and_or_xor", r"^(and|or|xor|not|bfn)$"),
    ("mov", r"^(mov|movi)$"),
    ("add_sub", r"^(add|addc|sub|subb|add3)$"),
    ("mul", r"^(mul|mach|macl|mac|madm|mullh)$"),
    ("mad_fp", r"^mad$"),
    ("math", r"^math"),
    ("cmp_sel", r"^(cmp|cmpn|sel|csel)$"),
    ("branch", r"^(jmpi|if|else|endif|while|break|cont|goto|join|brd|brc|call|ret)$"),
    ("sync", r"^sync"),
    ("send_other", r"^(send|sendc|sends|sendsc)|^(barrier|fence|atomic)"),
]

def classify(m):
    for name, pat in CLASSES:
        if re.search(pat, m):
            return name
    return "other:" + m

def parse(path):
    hdr, blocks, cur, spill_ops = {}, collections.OrderedDict(), None, 0
    for line in open(path, errors="replace"):
        s = line.strip()
        if s.startswith("//."):
            m = re.match(r"//\.(\S+)\s*(.*)", s)
            if m and m.group(1) in ("thread_config", "instCount", "platform", "spill", "spillGRFs", "spill_size", "spillMemUsed", "numFillInst", "numSpillInst", "RA"):
                hdr[m.group(1)] = m.group(2)
            if s.startswith("//.spill"):
                hdr.setdefault("spill_lines", []).append(s)
            continue
        m = re.match(r"// (B\d+): ", s)
        if m:
            cur = m.group(1); blocks[cur] = collections.Counter(); continue
        if not s or s.startswith("//") or s.endswith(":"):
            continue
        s2 = re.sub(r"^\([^)]*\)\s*", "", s)           # predicate / (W) prefix
        mn = s2.split()[0] if s2.split() else ""
        if not re.match(r"^[a-z_][a-z0-9_.]*$", mn):
            continue
        cls = classify(mn)
        if re.search(r"scratch|spill|fill", s, re.I) and cls in ("usm_block_load", "usm_gather", "usm_store", "send_other"):
            cls = "scratch_fill" if "load" in mn or "fill" in s.lower() else "scratch_spill"
        if cur is None:
            cur = "B?"; blocks[cur] = collections.Counter()
        blocks[cur][cls] += 1
        # width of the operation: (16|M0) etc.
        w = re.search(r"\((\d+)\|M\d+\)", s)
        blocks[cur]["_lanes:" + (w.group(1) if w else "?")] += 1
    return hdr, blocks

def show(title, c):
    tot = sum(v for k, v in c.items() if not k.startswith("_"))
    print(f"  {title}: {tot} instructions")
    for k, v in sorted(((k, v) for k, v in c.items() if not k.startswith("_")), key=lambda kv: -kv[1]):
        print(f"    {k:18s} {v}")
    print("    widths: " + ", ".join(f"{k[7:]}-wide x{v}" for k, v in sorted(c.items()) if k.startswith("_lanes:")))

def main():
    if len(sys.argv) < 2:
        print(__doc__); return 1
    d, subs = sys.argv[1], sys.argv[2:]
    for p in sorted(glob.glob(os.path.join(d, "*.asm"))):
        name = os.path.basename(p)
        if subs and not any(x in name for x in subs):
            continue
        hdr, blocks = parse(p)
        k = re.sub(r"^VC_asm[0-9a-f]+_+|^OCL_asm[0-9a-f]+_+", "", name)
        print(f"== {k}")
        for h in ("platform", "thread_config", "instCount", "RA"):
            if h in hdr: print(f"  {h}: {hdr[h]}")
        for s in hdr.get("spill_lines", [])[:6]: print("  " + s)
        total = collections.Counter()
        for b in blocks.values(): total.update(b)
        big = max(blocks.items(), key=lambda kv: sum(v for k2, v in kv[1].items() if not k2.startswith("_")))
        show(f"largest basic block {big[0]} (unrolled loop body)", big[1])
        show("whole kernel", total)
    return 0

if __name__ == "__main__":
    sys.exit(main())
