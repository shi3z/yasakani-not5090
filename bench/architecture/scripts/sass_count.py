#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Count SASS instructions by class in the hot loop(s) of a CUDA kernel.

Input: a listing written by `cuobjdump -sass <object>` (see cuda_codegen.sh).  Nothing is executed on a GPU.

    sass_count.py LISTING --list                         # mangled names of all functions
    sass_count.py LISTING -f REGEX [--elf ELF.txt]       # analyse the function(s) whose mangled name matches
                  [--loops mma|innermost|all]            # which loops to report (default: mma)
                  [--weight 0xADDR=W ...]                # execution weight of the basic block that contains ADDR
                  [--weight 0xA-0xB=W ...]               # ... of every basic block that starts in [A, B]
                  [--range 0xA:0xB ...]                  # count an explicit address range instead of detected loops
                  [--brief] [--blocks] [--dump] [--extract]

Loop detection: every branch (BRA/JMP/BRX with a literal target) whose target address is <= its own address is a
back edge; the loop body is the address range [target, branch].  This is exact for the reducible, layout-ordered
code that the compiler emits for these kernels, and the boundaries are printed so that they can be checked by eye.
  --loops mma        the smallest loops that contain a tensor-core instruction (default)
  --loops innermost  loops that contain no other loop
  --loops all        every loop

Counts are STATIC: one count per instruction in the address range, per thread (a SASS instruction is executed by
each of the 32 threads of a warp; an MMA instruction is one warp-wide operation written once per thread).
Predicated instructions (@P0 ...) count as one.  Blocks inside a loop that are skipped on some trips (forward
branch over them) are listed; give `--weight 0xADDR=0.5` to weight such a block, and the "weighted" column then
is the expected number of executed instructions per trip.

Register count, frame size and stack size come from the `.nv.info` attributes when `--elf` (output of
`cuobjdump -elf <object>`) is given; otherwise only the highest general register that appears in the code is shown.
"""
import argparse
import collections
import re
import sys

INSN_RE = re.compile(r"/\*([0-9a-fA-F]{4,})\*/\s+(.*?)\s*;")
FUNC_RE = re.compile(r"^\s*Function\s*:\s*(\S+)")
TARGET_RE = re.compile(r"(?:^|[\s,])(0x[0-9a-fA-F]+)\s*$")
REG_RE = re.compile(r"(?<![A-Za-z0-9_])R(\d+)(?![0-9A-Za-z_\[])")

CLASSES = [
    ("tensor", "tensor-core (IMMA/HMMA ...)"),
    ("smem_load", "shared-memory load (LDS, LDSM)"),
    ("smem_store", "shared-memory store (STS)"),
    ("gmem_load", "global/generic load (LDG, LD)"),
    ("gmem_store", "global/generic store (STG, ST)"),
    ("const_load", "constant-bank load (LDC, LDCU)"),
    ("spill_fill", "stack load/store via R1 (LDL/STL, [R1+..])"),
    ("shift_bit", "shift / bit-field (SHF SHL SHR BMSK LOP LOP3 SGXT ...)"),
    ("imad_pow2", "IMAD by 2^n immediate, IMAD.SHL (= shift[+add])"),
    ("imul", "integer multiply 32 bit (IMAD IMUL, non 2^n)"),
    ("imul_wide", "integer multiply 32x32->64 (+64-bit add) (IMAD.WIDE)"),
    ("add64", "64-bit add (IADD.64, carry forms .X)"),
    ("add32", "32-bit add / LEA (IADD IADD3 LEA)"),
    ("mov", "move / clear (MOV UMOV CS2R R2UR)"),
    ("cmp_sel", "compare / select / predicate (ISETP SEL PLOP3 ...)"),
    ("float", "floating point (FMUL FADD FFMA F2I I2F MUFU ...)"),
    ("shuffle", "warp shuffle / vote (SHFL VOTE)"),
    ("branch", "branch / call / exit (BRA CALL RET EXIT BSSY BSYNC)"),
    ("barrier", "barrier / async copy (BAR LDGSTS LDGDEPBAR DEPBAR ...)"),
    ("atomic", "atomic (ATOM ATOMG ATOMS RED REDG)"),
    ("nop", "padding (NOP, never-true predicate)"),
    ("other", "everything else"),
]
CLASS_KEYS = [k for k, _ in CLASSES]

SHIFT_OPS = {"SHF", "SHL", "SHR", "BMSK", "LOP", "LOP3", "LOP32I", "SGXT", "POPC", "FLO", "BREV",
             "USHF", "USHL", "USHR", "ULOP", "ULOP3", "ULOP32I", "UBMSK", "USGXT", "UPOPC", "UFLO", "UBREV"}
MOV_OPS = {"MOV", "UMOV", "MOV32I", "UMOV32I", "CS2R", "R2UR", "UR2R"}
CMP_OPS = {"ISETP", "UISETP", "FSETP", "DSETP", "SEL", "USEL", "FSEL", "PLOP3", "UPLOP3", "ICMP", "FCMP",
           "VIMNMX", "UVIMNMX", "IMNMX", "FMNMX", "CSET", "PSETP", "P2R", "R2P", "UP2UR", "UR2UP"}
FLOAT_OPS = {"FMUL", "FADD", "FFMA", "FSUB", "F2I", "UF2I", "I2F", "UI2F", "I2FP", "UI2FP", "F2IP", "UF2IP",
             "MUFU", "FCHK", "F2F", "DMUL", "DADD", "DFMA", "FRND", "FSWZADD", "HADD2", "HMUL2", "HFMA2",
             "FADD2", "FMUL2", "FFMA2", "F2FP"}
BRANCH_OPS = {"BRA", "BRX", "JMP", "JMX", "CALL", "RET", "EXIT", "BSSY", "BSYNC", "BREAK", "KILL", "BPT", "TRAP"}
BARRIER_OPS = {"BAR", "LDGSTS", "LDGDEPBAR", "DEPBAR", "WARPSYNC", "ACQBULK", "PREEXIT", "NANOSLEEP",
               "MEMBAR", "YIELD", "ACQSHMINIT", "LDGMC"}
ATOMIC_OPS = {"ATOM", "ATOMG", "ATOMS", "RED", "REDG", "REDS"}
ADD_OPS = {"IADD", "IADD3", "UIADD3", "UIADD", "LEA", "ULEA", "ISUB", "IABS", "INEG", "ISCADD", "IADD32I"}
JUMP_OPS = {"BRA", "BRX", "JMP", "JMX"}


class Insn:
    __slots__ = ("addr", "pred", "op", "base", "mods", "operands", "text", "cls", "target")

    def __init__(self, addr, text):
        self.addr = addr
        self.text = text
        t = text.strip()
        self.pred = None
        if t.startswith("@"):
            self.pred, _, t = t.partition(" ")
            t = t.strip()
        self.op, _, self.operands = t.partition(" ")
        self.operands = self.operands.strip()
        parts = self.op.split(".")
        self.base = parts[0]
        self.mods = parts[1:]
        self.target = None
        if self.base in JUMP_OPS or self.base == "CALL":
            m = TARGET_RE.search(self.operands)
            if m:
                self.target = int(m.group(1), 16)
        self.cls = classify(self)


def _is_pow2_imm(tok):
    tok = tok.strip()
    if not re.fullmatch(r"-?0x[0-9a-fA-F]+", tok):
        return False
    v = int(tok, 16)
    return v > 0 and (v & (v - 1)) == 0


def classify(i):
    b, mods, ops = i.base, i.mods, i.operands
    if i.pred in ("@!UPT", "@!PT") or b == "NOP":
        return "nop"
    if "MMA" in b:
        return "tensor"
    if b in BARRIER_OPS:
        return "barrier"
    if b in ATOMIC_OPS:
        return "atomic"
    is_load = b in ("LD", "LDG", "LDS", "LDL", "LDSM")
    is_store = b in ("ST", "STG", "STS", "STL")
    if is_load or is_store:
        if b in ("LDL", "STL") or re.search(r"\[R1(?![0-9])", ops):
            return "spill_fill"
        if b in ("LDS", "LDSM"):
            return "smem_load"
        if b == "STS":
            return "smem_store"
        return "gmem_load" if is_load else "gmem_store"
    if b in ("LDC", "LDCU"):
        return "const_load"
    if b in ("IMAD", "UIMAD", "IMUL", "UIMUL", "IMUL32I", "IMAD32I"):
        srcs = [s.strip() for s in ops.split(",")]
        if "SHL" in mods or (len(srcs) >= 3 and _is_pow2_imm(srcs[2])):
            return "imad_pow2"
        return "imul_wide" if "WIDE" in mods else "imul"
    if b in ADD_OPS:
        if "64" in mods or "X" in mods:
            return "add64"
        return "add32"
    if b in SHIFT_OPS:
        return "shift_bit"
    if b in MOV_OPS:
        return "mov"
    if b in CMP_OPS:
        return "cmp_sel"
    if b in FLOAT_OPS:
        return "float"
    if b in ("SHFL", "VOTE", "VOTEU"):
        return "shuffle"
    if b in BRANCH_OPS:
        return "branch"
    return "other"


def parse_listing(path):
    """Returns an ordered dict: mangled name -> list of Insn."""
    funcs = collections.OrderedDict()
    cur = None
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            m = FUNC_RE.match(line)
            if m:
                cur = funcs.setdefault(m.group(1), [])
                continue
            if cur is None:
                continue
            m = INSN_RE.search(line)
            if m:
                cur.append(Insn(int(m.group(1), 16), m.group(2)))
    return funcs


def extract_text(path, names):
    out, keep = [], False
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            m = FUNC_RE.match(line)
            if m:
                keep = m.group(1) in names
            if keep:
                out.append(line)
    return "".join(out)


def parse_elf(path):
    """Per-function attributes from `cuobjdump -elf` (.nv.info)."""
    info = collections.defaultdict(dict)
    val_re = re.compile(r"function:\s*(\S+?)\(0x[0-9a-fA-F]+\)\s+(register count|frame size|min stack size|max stack size)\s*:\s*(\S+)")
    sect_re = re.compile(r"^\.nv\.info\.(\S+)")
    cur, attr = None, None
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            m = val_re.search(line)
            if m:
                info[m.group(1)][m.group(2)] = m.group(3)
                continue
            m = sect_re.match(line)
            if m:
                cur, attr = m.group(1), None
                continue
            if line.startswith(".") and not line.startswith(".nv.info."):
                cur = None
            if cur is None:
                continue
            s = line.strip()
            if s.startswith("Attribute:"):
                attr = s.split()[-1]
            elif s.startswith("Value:") and attr in ("EIATTR_MAX_THREADS", "EIATTR_CRS_STACK_SIZE", "EIATTR_NUM_BARRIERS",
                                                      "EIATTR_MAXREG_COUNT", "EIATTR_MAX_STACK_SIZE"):
                info[cur][attr] = s[len("Value:"):].strip()
    return info


def find_loops(insns):
    """Back edges -> loops (start address, end address = address of the back branch)."""
    loops = set()
    for i in insns:
        if i.base in JUMP_OPS and i.target is not None and i.target <= i.addr:
            if i.target == i.addr and not i.pred:
                continue  # trailing self-branch that pads the function
            loops.add((i.target, i.addr))
    # merge several back edges to one header
    by_head = {}
    for s, e in loops:
        by_head[s] = max(by_head.get(s, e), e)
    return sorted(by_head.items())


def in_range(insns, s, e):
    return [i for i in insns if s <= i.addr <= e]


def blocks_of(insns, body):
    """Basic blocks of a loop body (leaders: loop start, branch targets, instruction after a branch)."""
    addrs = [i.addr for i in body]
    aset = set(addrs)
    leaders = {addrs[0]}
    for k, i in enumerate(body):
        if i.base in JUMP_OPS or i.base in ("CALL", "RET", "EXIT"):
            if k + 1 < len(body):
                leaders.add(body[k + 1].addr)
    for i in insns:
        if i.target is not None and i.target in aset and i.base != "CALL":
            leaders.add(i.target)
    blocks, cur = [], []
    for i in body:
        if i.addr in leaders and cur:
            blocks.append(cur)
            cur = []
        cur.append(i)
    if cur:
        blocks.append(cur)
    return blocks


def counts(seq, weight_of=None):
    c = collections.OrderedDict((k, 0.0) for k in CLASS_KEYS)
    for i in seq:
        c[i.cls] += 1.0 if weight_of is None else weight_of(i.addr)
    return c


def fmt(x):
    return ("%d" % x) if abs(x - round(x)) < 1e-9 else ("%.2f" % x)


def report_function(name, insns, args, elf):
    out = []
    P = out.append
    P("=" * 110)
    P("function: %s" % name)
    P("  instructions (static, whole function): %d   code bytes: 0x%x" % (len(insns), insns[-1].addr + 16 if insns else 0))
    regs = [int(m) for i in insns for m in REG_RE.findall(i.operands)]
    P("  highest general register that appears: R%d" % (max(regs) if regs else -1))
    whole_spill = [i for i in insns if i.cls == "spill_fill"]
    P("  stack loads/stores via R1 in the whole function: %d%s" % (
        len(whole_spill), "" if not whole_spill else "  at " + " ".join("0x%04x" % i.addr for i in whole_spill[:12])))
    if elf is not None:
        e = elf.get(name, {})
        if e:
            names = [("register count", "register count"), ("frame size", "frame size"), ("min stack size", "min stack size"),
                     ("EIATTR_CRS_STACK_SIZE", "CRS stack"), ("EIATTR_MAX_THREADS", "max threads"), ("EIATTR_NUM_BARRIERS", "barriers")]
            P("  .nv.info: " + ", ".join("%s %s" % (lbl, e[k]) for k, lbl in names if k in e))
        else:
            P("  .nv.info: no entry for this function in the ELF listing")
    whole = counts(insns)
    P("  whole function by class: " + ", ".join("%s %s" % (k, fmt(v)) for k, v in whole.items() if v))

    loops = find_loops(insns)
    P("  loops (back edges), [start, back-branch], instructions, tensor-core instructions:")
    info = []
    for s, e in loops:
        body = in_range(insns, s, e)
        nm = sum(1 for i in body if i.cls == "tensor")
        children = [(s2, e2) for s2, e2 in loops if (s2, e2) != (s, e) and s <= s2 and e2 <= e]
        info.append((s, e, body, nm, children))
        P("    [0x%04x, 0x%04x]  %5d insns  %3d mma  %d nested loop(s)" % (s, e, len(body), nm, len(children)))
    if args.loops == "all":
        chosen = info
    elif args.loops == "innermost":
        chosen = [x for x in info if not x[4]]
    else:
        chosen = []
        for x in info:
            if x[3] == 0:
                continue
            kids_with = [c for c in info if (c[0], c[1]) in x[4] and c[3] > 0]
            if not kids_with:
                chosen.append(x)
    for r in args.range or []:
        a, _, b = r.partition(":")
        s, e = int(a, 16), int(b, 16)
        body = in_range(insns, s, e)
        if body:
            chosen.append((s, e, body, sum(1 for i in body if i.cls == "tensor"),
                           [(s2, e2) for s2, e2 in loops if s <= s2 and e2 <= e and (s2, e2) != (s, e)]))
    if args.range:
        chosen = [x for x in chosen if any(("%x:%x" % (x[0], x[1])) == "%x:%x" % (int(r.partition(":")[0], 16), int(r.partition(":")[2], 16)) for r in args.range)]
    if not chosen:
        P("  no loop selected (--loops %s)" % args.loops)

    weights = []
    for w in args.weight or []:
        a, _, v = w.partition("=")
        lo, _, hi = a.partition("-")
        weights.append((int(lo, 16), int(hi, 16) if hi else None, float(v)))

    for s, e, body, nm, children in chosen:
        P("-" * 110)
        P("  %s [0x%04x, 0x%04x]: %d instructions per thread (static), %d tensor-core instruction(s), nested loops: %s" % (
            "LOOP" if (s, e) in loops else "RANGE (given with --range, not a detected loop)", s, e, len(body), nm,
            " ".join("[0x%04x,0x%04x]" % c for c in children) or "none"))
        P("    first: 0x%04x  %s" % (body[0].addr, body[0].text))
        P("    last:  0x%04x  %s" % (body[-1].addr, body[-1].text))
        blocks = blocks_of(insns, body)
        bw = {}
        for b in blocks:
            w = 1.0
            for lo, hi, v in weights:
                if (hi is None and b[0].addr <= lo <= b[-1].addr) or (hi is not None and lo <= b[0].addr <= hi):
                    w = v
            for i in b:
                bw[i.addr] = w
        fwd = [i for i in body if i.base in JUMP_OPS and i.target is not None and i.target > i.addr]
        P("    basic blocks (start..end, instructions, mma, weight, remark):")
        small_n = small_i = 0
        for b in blocks:
            b0, b1 = b[0].addr, b[-1].addr
            if args.brief and len(b) < args.brief and bw[b0] == 1.0 and not any(i.cls == "tensor" for i in b):
                small_n += 1
                small_i += len(b)
                continue
            skip = [i for i in fwd if i.addr < b0 and i.target > b1]
            rem = ""
            if skip:
                near = skip[-1]
                rem = "can be skipped: forward branch at 0x%04x (%s%s)" % (near.addr, (near.pred + " ") if near.pred else "", near.op)
                if len(skip) > 1:
                    rem += " and %d outer" % (len(skip) - 1)
            if any(cs <= b0 and b1 <= ce for cs, ce in children):
                rem = (rem + "; " if rem else "") + "inside nested loop"
            P("      0x%04x..0x%04x  %4d  %2d  w=%s  %s" % (b0, b1, len(b), sum(1 for i in b if i.cls == "tensor"), fmt(bw[b0]), rem))
            if args.blocks:
                bc = counts(b)
                P("          " + ", ".join("%s %s" % (k, fmt(v)) for k, v in bc.items() if v))
        if small_n:
            P("      (+ %d blocks shorter than %d instructions with weight 1, %d instructions in total, not listed)" % (small_n, args.brief, small_i))
        st = counts(body)
        wt = counts(body, lambda a: bw[a])
        P("    %-62s %8s %9s   opcodes" % ("class", "static", "weighted"))
        for k, label in CLASSES:
            if not st[k]:
                continue
            hist = collections.Counter(i.op for i in body if i.cls == k)
            hs = " ".join("%s:%d" % (o, n) for o, n in hist.most_common(8))
            P("    %-62s %8s %9s   %s" % (label, fmt(st[k]), fmt(wt[k]), hs))
        P("    %-62s %8s %9s" % ("TOTAL", fmt(sum(st.values())), fmt(sum(wt.values()))))
        pred = sum(1 for i in body if i.pred and i.cls != "nop")
        P("    predicated instructions in the loop (counted as 1 each): %d" % pred)
        sp = [i for i in body if i.cls == "spill_fill"]
        P("    stack loads/stores via R1 in the loop: %d" % len(sp))
        lr = [int(m) for i in body for m in REG_RE.findall(i.operands)]
        P("    distinct general registers referenced in the loop: %d (highest R%d)" % (len(set(lr)), max(lr) if lr else -1))
        if args.dump:
            for i in body:
                P("      /*%04x*/ %-5s %s   [%s]" % (i.addr, i.pred or "", (i.op + " " + i.operands).strip(), i.cls))
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("listing", help="output of cuobjdump -sass")
    ap.add_argument("-f", "--func", action="append", help="regular expression searched in the mangled function name (repeatable)")
    ap.add_argument("--list", action="store_true", help="list the functions and exit")
    ap.add_argument("--elf", help="output of cuobjdump -elf for the same object (register count, frame size)")
    ap.add_argument("--loops", choices=["mma", "innermost", "all"], default="mma")
    ap.add_argument("--weight", action="append", metavar="0xADDR=W", help="execution weight per trip of the basic block containing ADDR (or 0xA-0xB=W: blocks starting in [A, B])")
    ap.add_argument("--range", action="append", metavar="0xA:0xB",
                    help="count this explicit address range (inclusive) instead of the detected loops, e.g. one part of a large loop body")
    ap.add_argument("--brief", type=int, nargs="?", const=10, default=0, metavar="N",
                    help="do not list unweighted basic blocks shorter than N instructions (default N = 10)")
    ap.add_argument("--blocks", action="store_true", help="print the class counts of every basic block of the loop")
    ap.add_argument("--dump", action="store_true", help="print the classified instructions of the loop")
    ap.add_argument("--extract", action="store_true", help="print the raw SASS of the matching functions and exit")
    args = ap.parse_args()

    funcs = parse_listing(args.listing)
    if args.list:
        for n, ins in funcs.items():
            print("%6d  %s" % (len(ins), n))
        return 0
    if not args.func:
        ap.error("give --func REGEX (or --list)")
    names = [n for n in funcs if any(re.search(p, n) for p in args.func)]
    if not names:
        print("no function matches", file=sys.stderr)
        return 1
    if args.extract:
        sys.stdout.write(extract_text(args.listing, set(names)))
        return 0
    elf = parse_elf(args.elf) if args.elf else None
    for n in names:
        print(report_function(n, funcs[n], args, elf))
    return 0


if __name__ == "__main__":
    sys.exit(main())
