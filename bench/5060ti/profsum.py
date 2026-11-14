#!/usr/bin/env python3
"""Summarize a YSKN_PROF=1 log: per row count, the kernels sorted by time (ms per pass)."""
import re, subprocess, collections, sys, bisect
log, lib = sys.argv[1], sys.argv[2]
syms = []
for l in subprocess.run(["nm", "-C", "--defined-only", lib], capture_output=True, text=True).stdout.splitlines():
    p = l.split(" ", 2)
    if len(p) == 3 and p[1] in "tTwW": syms.append((int(p[0], 16), p[2]))
syms.sort(); addrs = [a for a, _ in syms]
def name(off):
    i = bisect.bisect_right(addrs, off) - 1
    s = syms[i][1] if i >= 0 else "?"
    s = re.sub(r"^void yskn::", "", s); s = re.sub(r"\(.*$", "", s); return s[:78]
rows = collections.defaultdict(list); passes = {}
for l in open(log):
    m = re.match(r"\[prof\] rows=(\d+) ms=([\d.]+) n=(\d+) (\S+)", l)
    if m: rows[int(m[1])].append((float(m[2]), int(m[3]), name(int(m[4], 16))))
top = int(sys.argv[3]) if len(sys.argv) > 3 else 14
for r in sorted(rows):
    v = sorted(rows[r], reverse=True); tot = sum(x[0] for x in v)
    if tot < 50: continue
    print("== rows %d: total %.0f ms" % (r, tot))
    for ms, n, s in v[:top]: print("  %8.1f ms %5.1f%%  n=%6d  %7.3f ms/call  %s" % (ms, 100 * ms / tot, n, ms / n, s))
