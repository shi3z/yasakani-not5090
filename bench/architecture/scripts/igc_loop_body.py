#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Print the (compacted) instructions of the self-looping basic blocks of IGC kernel assembly dumps:
#   scripts/igc_loop_body.py <dump dir> <substring of the kernel name> [...]     (see igc_asm_count.py for how to dump)
import sys,re,glob
# print compact loop body: the basic block that is its own predecessor (smallest such), for files matching substrings
d=sys.argv[1]; subs=sys.argv[2:]
for p in sorted(glob.glob(d+'/*.asm')):
    if 'RoundedRange' in p: continue
    if not all(x in p for x in subs): continue
    blocks={}; cur=None; preds={}
    for line in open(p,errors='replace'):
        m=re.match(r'// (B\d+): Preds:\{([^}]*)\}',line)
        if m: cur=m.group(1); blocks[cur]=[]; preds[cur]=[x.strip() for x in m.group(2).split(',')]; continue
        s=line.strip()
        if cur and s and not s.startswith('//') and not s.endswith(':'): blocks[cur].append(s)
    self_=[b for b in blocks if b in preds[b]]
    print('==',re.sub(r'^.*asm[0-9a-f]+_+','',p)[:90], 'self-loop blocks:',[(b,len(blocks[b])) for b in self_])
    for b in self_[:2]:
        for s in blocks[b][:60]:
            s=re.sub(r'\s+//.*$','',s); s=re.sub(r'\{[^}]*\}','',s); print('   ',re.sub(r'\s+',' ',s)[:120])
