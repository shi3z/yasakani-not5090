# Is the selfcheck mismatch of the quality model a near-tie?  Generate greedily, log top-2 margins, then try the runner-up
# at the closest positions and compare the sha256 of the resulting ids with the reference.
import os, sys, json, hashlib, numpy as np
MD = sys.argv[1]; DLL = sys.argv[2]; CASE = sys.argv[3] if len(sys.argv) > 3 else 'thinking'
LOG = '/data/yasakani/tmp/margins.txt'
os.environ['YX_NO_LOOKAHEAD'] = '1'; os.environ['YX_MARGIN_LOG'] = LOG
from yasakani.runtime import ModelPack
from yasakani.engine import Engine, Sampling
from yasakani.textio import ChatFormatter
from yasakani import cli
pack = ModelPack.load(MD); eng = Engine(pack, max_ctx=8192, dll=DLL); fmt = ChatFormatter(pack.root)
ref = {c['name']: c for c in json.load(open(MD + '/selfcheck.json'))['cases']}[CASE]
c = [x for x in cli._selfcheck_cases() if x['name'] == CASE][0]
prompt = fmt.encode(c['text']) if c['kind'] == 'raw' else fmt.prompt(c['messages'], **c['template_kwargs'])[0]
prompt = [int(x) for x in prompt]; n = c['max_new']
sha = lambda ids: hashlib.sha256(np.asarray(ids, '<i4').tobytes()).hexdigest()
def gen(p, k):
    eng.set_sampling(Sampling(temperature=0.0)); eng.reset(); open(LOG, 'w').close()
    return [int(x) for x in eng.generate(p, k).ids]
ids = gen(prompt, n)
print('tokens', len(ids), 'head equal to reference:', ids[:12] == ref['head'], ' sha equal:', sha(ids) == ref['sha256'])
rows = [l.split() for l in open(LOG)]; m = {}
for r in rows: m[int(r[0])] = (int(r[1]), float(r[2]), int(r[3]), float(r[4]))
P = len(prompt); cand = []
for k in range(len(ids)):
    t, v1, t2, v2 = m[P + k]; assert t == ids[k], (k, t, ids[k]); cand.append((v1 - v2, k, t2))
cand.sort(); print('smallest margins (margin, index, chosen -> runner-up):', [(round(a, 5), k, ids[k], t2) for a, k, t2 in cand[:8]])
print('text:', repr(fmt.decode(ids))[:300])
for a, k, t2 in cand[:6]:
    alt = ids[:k] + [t2]; rest = gen(prompt + alt, n - len(alt)) if n - len(alt) > 0 else []
    full = alt + rest; ok = sha(full) == ref['sha256']
    print('flip at index %d (margin %.5f): %d tokens, sha equal to reference: %s' % (k, a, len(full), ok), flush=True)
    if ok: print('  -> the reference output is ours with the runner-up token at index', k); break
