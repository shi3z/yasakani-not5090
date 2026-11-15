# Teacher-forced single-layer check: feed the oracle hidden state after layer l-1, run layer l only, compare with the
# oracle after layer l.  Isolates the error of one layer from the accumulated drift.  (Prefill from position 0 only:
# the GDN state / KV cache start empty.)   usage: run_tf.py <oracle> <layer> [<layer> ...]
import sys, time, numpy as np
sys.path.insert(0, '.')
from ref.core import *
o = np.load('work/oracle/%s.npz' % sys.argv[1]); ids = o['ids']; M = len(ids); LY = list(o['layers'])
def is_attn(l): return l % 4 == 3
def first_in(l): return Tensor('layers.%d.linear_attn.in_proj_qkv' % l) if not is_attn(l) else Tensor('layers.%d.self_attn.q_proj' % l)
def one(l, keep=None):
    assert l in LY and (l == 0 or l - 1 in LY), 'need captures of layers l-1 and l'
    t0 = time.time(); st = State(); st.h = embed(ids) if l == 0 else o['caps'][LY.index(l - 1)].copy()
    res_norm(st, None, vec('layers.%d.input_layernorm' % l), first_in(l).sg_in, want_xn=not is_attn(l))
    blk = Attn(l) if is_attn(l) else GDN(l)
    T = blk.forward(st, M, 0) if is_attn(l) else blk.forward(st, M)
    mlp = MLP(l); res_norm(st, T, vec('layers.%d.post_attention_layernorm' % l), mlp.gate.sg_in); T = mlp.forward(st)
    st.h = rbf(st.h + rbf(rot_out(yr(st.acc[T.name], st.us, T.tau), T.sg_out)))
    ref = o['caps'][LY.index(l)]; rr = np.linalg.norm(st.h - ref, axis=1) / np.linalg.norm(ref, axis=1)
    print('layer %2d%s forced: rel %.3e  exact-equal %.4f  rows exact %s  row rel max %.3e (row %d)  (%.0f s)' % (l, ' A' if is_attn(l) else '  ',
          np.linalg.norm(st.h - ref) / np.linalg.norm(ref), (st.h == ref).mean(), ''.join('1' if (st.h[i] == ref[i]).all() else '0' for i in range(M)), rr.max(), rr.argmax(), time.time() - t0), flush=True)
    print('          per-row exact fraction: ' + ' '.join('%.3f' % (st.h[i] == ref[i]).mean() for i in range(M)), flush=True)
    return blk, st
if __name__ == '__main__':
    for a in sys.argv[2:]: one(int(a))
