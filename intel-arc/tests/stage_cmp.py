import sys, numpy as np
sys.path.insert(0, '.')
from ref.core import *
ids = np.fromfile('work/ids.bin', np.int32); M = len(ids)
def ld(n, dt, shape): return np.fromfile('work/st_%s.bin' % n, dt).reshape(shape)
def cmp(name, a, b): print('%-10s equal %s  mismatches %d / %d  maxdiff %s' % (name, bool((a == b).all()), int((a != b).sum()), a.size, np.abs(a.astype(np.float64) - b.astype(np.float64)).max()))
st = State(); st.h = embed(ids); cmp('emb', ld('emb', f32, (M, H)), st.h)
qkv = Tensor('layers.0.linear_attn.in_proj_qkv'); zt = Tensor('layers.0.linear_attn.in_proj_z')
res_norm(st, None, vec('layers.0.input_layernorm'), qkv.sg_in, want_xn=True)
cmp('S', ld('S', f32, (16, 16))[:M, :10], st.S); cmp('alpha', ld('alpha', f32, (M, 10)), st.alpha)
qx = ld('qx', np.int8, (160, 16, 32)).transpose(1, 0, 2).reshape(16, H)[:M]; cmp('qx', qx, st.qx)
cmp('xn', ld('xn', np.uint16, (M, H)), (st.xn.view(np.uint32) >> 16).astype(np.uint16))
run_gemv(st, [qkv]); cmp('us', ld('us', f32, (M,)), st.us)
cmp('acc_qkv', ld('acc_qkv', np.int64, (16, 10240))[:M], st.acc[qkv.name])
run_gemv(st, [zt]); cmp('acc_z', ld('acc_z', np.int64, (16, 6144))[:M], st.acc[zt.name])
a = ld('acc_qkv', np.int64, (16, 10240)); b = st.acc if False else None
run_gemv(st, [qkv]); r = st.acc[qkv.name]
print('qkv per-row mismatch frac', (a[:M] != r).mean(1).round(3))
print('qkv row0 first 4:', a[0, :4], r[0, :4], ' rows 12..15 abs max', np.abs(a[12:]).max())
az = ld('acc_z', np.int64, (16, 6144)); run_gemv(st, [zt]); rz = st.acc[zt.name]
print('z per-row mismatch frac', (az[:M] != rz).mean(1).round(3))
print('Aw', ld('Aw', np.int32, (16, 10))[:2], 'ref A', np.rint(st.alpha * (f32(16777216) / st.alpha.max(-1))[:, None])[:2].astype(np.int64))
print('is qkv dump == z-like?', (a[:M, :6144] == rz).mean())
