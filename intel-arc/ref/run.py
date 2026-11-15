# usage: run.py <number of layers> [oracle name]      (prefill pass of the oracle prompt, compare hidden states)
import sys, time, numpy as np
sys.path.insert(0, '.')
from ref.core import *
o = np.load('work/oracle/%s.npz' % (sys.argv[2] if len(sys.argv) > 2 else 'a')); ids = o['ids']; M = len(ids)
upto = int(sys.argv[1]) if len(sys.argv) > 1 else 3
st = State(); st.h = embed(ids)
def is_attn(l): return l % 4 == 3
def first_in(l): return Tensor('layers.%d.linear_attn.in_proj_qkv' % l) if not is_attn(l) else Tensor('layers.%d.self_attn.q_proj' % l)
blocks = {}                                         # per-layer objects hold the recurrent state / KV cache across passes
pos0 = 0                                            # prefill: first slot of the pass
res_norm(st, None, vec('layers.0.input_layernorm'), first_in(0).sg_in, want_xn=True)
for l in range(upto):
    t0 = time.time()
    if is_attn(l): blk = blocks[l] = Attn(l); T = blk.forward(st, M, pos0)
    else: blk = blocks[l] = GDN(l); T = blk.forward(st, M)
    mlp = MLP(l)
    res_norm(st, T, vec('layers.%d.post_attention_layernorm' % l), mlp.gate.sg_in)
    T = mlp.forward(st)
    nxt = l + 1
    if nxt < 64: res_norm(st, T, vec('layers.%d.input_layernorm' % nxt), first_in(nxt).sg_in, want_xn=not is_attn(nxt))
    else: st.h = rbf(st.h + rbf(rot_out(yr(st.acc[T.name], st.us, T.tau), T.sg_out)))      # last layer: residual only (final norm / lm_head not done here)
    k = list(o['layers']).index(l) if l in o['layers'] else -1
    if k >= 0:
        ref = o['caps'][k]; d = np.abs(st.h - ref); rr = np.linalg.norm(st.h - ref, axis=1) / np.linalg.norm(ref, axis=1)
        print('layer %2d%s: max|diff| %.3e  rel %.3e  exact-equal %.4f  worst-row rel %.3e (row %d)  (%.0f s)' % (l, ' A' if is_attn(l) else '  ', d.max(),
              np.linalg.norm(st.h - ref) / np.linalg.norm(ref), (st.h == ref).mean(), rr.max(), rr.argmax(), time.time() - t0), flush=True)
    else: print('layer %2d%s: (no capture)  (%.0f s)' % (l, ' A' if is_attn(l) else '  ', time.time() - t0), flush=True)
    np.save('work/ref%s_h_%d.npy' % ('q' if QUAL else '', l), st.h)
