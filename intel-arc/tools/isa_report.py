#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Per-kernel facts from an IGC shader dump (Xe / Gen assembly), as CSV or Markdown, and the difference between two dumps.

Dump (kernels are compiled when the program is first used; the caches must be off or nothing is compiled):
  NEO_CACHE_PERSISTENT=0 SYCL_CACHE_PERSISTENT=0 IGC_ShaderDumpEnable=1 IGC_DumpToCustomDir=<dir>/ <any run that loads the library>
Report:
  tools/isa_report.py <dir> [--match substr[,substr]] [--csv out.csv] [--md out.md]
  tools/isa_report.py <dir_before> --diff <dir_after> [--match ...] [--md out.md]

All counts are STATIC (instructions in the binary), not executed instructions.  Classification:
  - a GRF spill / fill is an instruction the compiler annotated "spill to offset" / "fill from offset" (it addresses the scratch
    surface ss[...]); ordinary load.ugm / store.ugm to user memory are counted separately and never as spills
  - flag spills are the "//.spill flag store|load" header counts (a flag register saved to / restored from a GRF, no memory)
  - barrier = sync.bar; fence = send to the SLM fence; gateway = send.gtwy
"""
import argparse, collections, csv, glob, os, re, sys

COLS = ["kernel", "variant", "simd", "numGRF", "numAcc", "insts", "RA", "spill_bytes", "grf_spill", "grf_fill", "flag_spill", "flag_fill",
        "dpas", "load_ugm", "store_ugm", "gather_ugm", "scatter_ugm", "load_slm", "store_slm", "gather_slm", "scatter_slm",
        "sync_bar", "send_gtwy", "fence", "math", "branches", "blocks", "max_block", "slm_bytes", "barriers_zeinfo", "private_bytes"]

def short(name):
    """A readable kernel name from the mangled lambda name."""
    wrapped = "RoundedRangeKernel" in name
    names = re.findall(r"(\d+)([A-Za-z_][A-Za-z0-9_]*)", name)
    # walk the nested-name pieces after 'yx': take identifiers until the lambda marker
    m = re.search(r"N2yx(.*?)E?U[l0-9]", name)
    body = m.group(1) if m else name
    parts = []; i = 0
    while i < len(body):
        mm = re.match(r"(\d+)", body[i:])
        if not mm: break
        n = int(mm.group(1)); i += len(mm.group(1)); parts.append(body[i:i + n]); i += n
        if body[i:i + 1] == "I":                       # template arguments: keep them compactly
            j = body.find("EE", i); targ = body[i:j + 1] if j > 0 else ""
            parts[-1] += "<" + ",".join(re.findall(r"L[a-z](\d+)E", targ)) + ">"; break
    lam = re.search(r"EUl.*?E(\d*)_", name)
    idx = lam.group(1) if lam and lam.group(1) else ""
    s = "::".join(p for p in parts if p not in ("fx", "ak")) or name[:60]
    return s + ("#" + idx if idx else "") + (" (rounded)" if wrapped else "")

def parse_asm(path):
    r = collections.Counter(); hdr = {}; cur = 0; blocks = 0; maxb = 0
    for line in open(path, errors="replace"):
        s = line.strip()
        if s.startswith("//."):
            m = re.match(r"//\.kernel (\S+)", s)
            if m: hdr["mangled"] = m.group(1)
            m = re.match(r"//\.thread_config numGRF=(\d+), numAcc=(\d+)", s)
            if m: hdr["numGRF"], hdr["numAcc"] = int(m.group(1)), int(m.group(2))
            m = re.match(r"//\.instCount (\d+)", s)
            if m: hdr["insts"] = int(m.group(1))
            m = re.match(r"//\.RA type\s+(\S+)", s)
            if m: hdr["RA"] = m.group(1)
            m = re.match(r"//\.spill size (\d+)", s)
            if m: hdr["spill_bytes"] = int(m.group(1))
            m = re.match(r"//\.spill flag store (\d+)", s)
            if m: hdr["flag_spill"] = int(m.group(1))
            m = re.match(r"//\.spill flag load (\d+)", s)
            if m: hdr["flag_fill"] = int(m.group(1))
            m = re.match(r"//\.private memory size (\d+)", s)
            if m: hdr["private_bytes"] = int(m.group(1))
            continue
        if re.match(r"// B\d+", s) or re.match(r"^[A-Za-z_.$][\w.$]*:$", s):
            blocks += 1; maxb = max(maxb, cur); cur = 0; continue
        if not s or s.startswith("//"): continue
        code, _, note = s.partition("//")
        t = re.sub(r"^\([^)]*\)\s*", "", code).split()
        if not t: continue
        mn = t[0]
        if not re.match(r"^[a-z_][a-z0-9_.]*$", mn): continue
        cur += 1
        scratch = "ss[" in code or re.search(r"spill to offset|fill from offset", s) is not None
        if mn.startswith("load.") or mn.startswith("store."):
            st = mn.startswith("store."); slm = ".slm." in mn
            block = re.search(r"\.d\d+x\d+t", mn) is not None
            if scratch and not slm: r["grf_spill" if st else "grf_fill"] += 1
            elif slm: r[("store_slm" if block else "scatter_slm") if st else ("load_slm" if block else "gather_slm")] += 1
            else: r[("store_ugm" if block else "scatter_ugm") if st else ("load_ugm" if block else "gather_ugm")] += 1
        elif mn.startswith("dpas"): r["dpas"] += 1
        elif mn == "sync.bar": r["sync_bar"] += 1
        elif mn.startswith("send") and "gtwy" in mn: r["send_gtwy"] += 1
        elif mn.startswith("send") or mn.startswith("fence") or mn.startswith("barrier") or mn.startswith("atomic"):
            if "fence" in s.lower() or mn.startswith("fence"): r["fence"] += 1
            elif "gtwy" in s.lower(): r["send_gtwy"] += 1
        elif mn.startswith("math"): r["math"] += 1
        elif mn in ("jmpi", "if", "else", "endif", "while", "break", "cont", "goto", "join", "brd", "brc", "call", "ret"): r["branches"] += 1
    maxb = max(maxb, cur)
    r.update({k: v for k, v in hdr.items() if k != "mangled" and k != "RA"})
    d = dict(r); d["RA"] = hdr.get("RA", ""); d["mangled"] = hdr.get("mangled", os.path.basename(path)); d["blocks"] = blocks; d["max_block"] = maxb
    return d

def zeinfo(dirname):
    info = {}
    for z in glob.glob(os.path.join(dirname, "*.zeinfo")):
        cur = None
        for line in open(z, errors="replace"):
            m = re.match(r"\s+- name:\s+(\S+)", line)
            if m: cur = m.group(1); info.setdefault(cur, {}); continue
            if cur is None: continue
            for key, col in (("slm_size", "slm_bytes"), ("barrier_count", "barriers_zeinfo"), ("simd_size", "simd")):
                m = re.match(r"\s+%s:\s+(\d+)" % key, line)
                if m: info[cur][col] = int(m.group(1))
    return info

def load(dirname, match):
    zi = zeinfo(dirname); rows = {}
    for p in sorted(glob.glob(os.path.join(dirname, "*.asm"))):
        d = parse_asm(p); name = d["mangled"]
        if match and not any(m in name for m in match): continue
        d.update(zi.get(name, {})); d["kernel"] = short(name)
        b = os.path.basename(p); d["variant"] = "esimd" if b.startswith("VC_") else (re.search(r"simd(\d+)", b).group(0) if re.search(r"simd(\d+)", b) else "")
        key = d["kernel"]; n = 2
        while key in rows: key = "%s [%d]" % (d["kernel"], n); n += 1
        d["kernel"] = key; rows[key] = d
    return rows

def table(rows, cols):
    out = ["| " + " | ".join(cols) + " |", "|" + "|".join("---" if c in ("kernel", "variant", "RA") else "---:" for c in cols) + "|"]
    for k in sorted(rows): out.append("| " + " | ".join(str(rows[k].get(c, 0 if c not in ("RA", "variant") else "")) for c in cols) + " |")
    return "\n".join(out)

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("dir"); ap.add_argument("--diff"); ap.add_argument("--match", default=""); ap.add_argument("--csv"); ap.add_argument("--md")
    ap.add_argument("--cols", default=""); a = ap.parse_args(); match = [m for m in a.match.split(",") if m]
    cols = a.cols.split(",") if a.cols else ["kernel", "variant", "numGRF", "insts", "spill_bytes", "grf_spill", "grf_fill", "flag_spill", "flag_fill", "dpas", "load_ugm", "store_ugm",
                                             "gather_ugm", "load_slm", "store_slm", "gather_slm", "sync_bar", "send_gtwy", "slm_bytes", "max_block"]
    A = load(a.dir, match)
    if a.csv:
        with open(a.csv, "w", newline="") as f:
            w = csv.writer(f); w.writerow(COLS)
            for k in sorted(A): w.writerow([A[k].get(c, 0 if c not in ("RA", "variant") else "") for c in COLS])
    if not a.diff:
        text = table(A, cols)
    else:
        B = load(a.diff, match); dc = [c for c in cols if c not in ("kernel", "variant")]; lines = ["| kernel | " + " | ".join(dc) + " |", "|---|" + "|".join("---:" for _ in dc) + "|"]
        for k in sorted(set(A) | set(B)):
            x, y = A.get(k), B.get(k)
            if x is None or y is None: lines.append("| %s | %s |" % (k, "only in the %s dump" % ("second" if x is None else "first"))); continue
            if all(x.get(c, 0) == y.get(c, 0) for c in dc): continue
            lines.append("| %s | " % k + " | ".join((str(x.get(c, 0)) if x.get(c, 0) == y.get(c, 0) else "%s -> %s" % (x.get(c, 0), y.get(c, 0))) for c in dc) + " |")
        text = "\n".join(lines) + "\n\n(kernels with identical counts are not listed; counts are static)"
    if a.md: open(a.md, "w").write(text + "\n")
    print(text)

if __name__ == "__main__": main()
