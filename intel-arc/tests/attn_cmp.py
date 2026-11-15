# Bit-exact comparison of tests/attn_test (SYCL) with ref/core.py class Attn.
#   attn_cmp.py gen   <tag> <layer> <nrows> [pos_start]        writes <tag>_in.bin (layer inputs) and, for pos_start > 0, a random cache prefix
#   attn_cmp.py chain <tag> <layer> <pos_start> <pass rows...>  after  attn_test chain ...  (layer 3: also compares with ref_h_2 / ref_h_3 of ref/run.py 4 a)
#   attn_cmp.py unit  <tag> <layer> <pos_start> <pass rows...>  after  attn_test unit ...
import sys, numpy as np
sys.path.insert(0, '/home/shi3z/yasakani-src/xe')
from ref.core import *
D = '/data/yasakani/tmp/attn/'
mode, tag, layer = sys.argv[1], D + sys.argv[2] + '_', int(sys.argv[3])
CN = lambda vpk: [('kp', '<u4', 40), ('kps', '<f4', 1), ('vp', '<u4', 40), ('vps', '<f4', 1)] if vpk else [('kp', '<u4', 40), ('kps', '<f4', 1), ('v8', 'i1', 256), ('vs', '<f4', 1)]
if mode == 'gen':
    n = int(sys.argv[4]); p0 = int(sys.argv[5]) if len(sys.argv) > 5 else 0
    base = np.load('/data/yasakani/tmp/ref_h_%d.npy' % (layer - 1)); rng = np.random.default_rng(1000 + layer)
    rows = [base[i] if i < len(base) else rbf((rng.standard_normal(len(base)).astype(f32)[:, None] * base).sum(0) / f32(np.sqrt(len(base)))) for i in range(n)]
    np.stack(rows).astype(f32).tofile(tag + 'in.bin')
    if p0:
        vpk = kv_packable(layer // 4, 1)
        sc = lambda lo, hi: rng.uniform(lo, hi, (4, p0)).astype(np.float16).astype(f32)
        rng.integers(0, 2**32, (4, p0, 40), dtype=np.uint32).tofile(tag + 'c_kp.bin'); sc(0.3, 1.2).tofile(tag + 'c_kps.bin')
        if vpk: rng.integers(0, 2**32, (4, p0, 40), dtype=np.uint32).tofile(tag + 'c_vp.bin'); sc(0.05, 2.0).tofile(tag + 'c_vps.bin')
        else: rng.integers(-127, 128, (4, p0, 256), dtype=np.int8).tofile(tag + 'c_v8.bin'); (sc(0.05, 2.0) / f32(127)).astype(f32).tofile(tag + 'c_vs.bin')
    print('wrote', tag + 'in.bin', n, 'rows', 'cache prefix %d' % p0 if p0 else ''); sys.exit(0)
p0 = int(sys.argv[4]); passes = [int(a) for a in sys.argv[5:]]; N = sum(passes); chain = mode == 'chain'
ok_all = True
def cmp(name, a, b):
    global ok_all
    a = np.asarray(a); b = np.asarray(b); assert a.shape == b.shape, (name, a.shape, b.shape)
    same = a.view(np.uint8 if a.dtype.itemsize == 1 else ('u%d' % a.dtype.itemsize)) == b.view(np.uint8 if b.dtype.itemsize == 1 else ('u%d' % b.dtype.itemsize))
    ok = bool(same.all()); ok_all &= ok
    print('%-8s %s  mismatches %d / %d%s' % (name, 'BIT-EXACT' if ok else 'DIFFERENT', int((~same).sum()), a.size,
          '' if ok else '  maxdiff %.3e  first at %s' % (np.nanmax(np.abs(a.astype(np.float64) - b.astype(np.float64))), np.argwhere(~same)[0].tolist())), flush=True)
ld = lambda n, dt, shape: np.fromfile(tag + n + '.bin', dt).reshape(shape)
blk = Attn(layer); mlp = MLP(layer) if chain else None
ln1, ln2 = vec('layers.%d.input_layernorm' % layer), vec('layers.%d.post_attention_layernorm' % layer)
rt = np.fromfile(tag + 'rope.bin', f32).reshape(-1, 64); cmp('rope', rt, np.concatenate(rope_cs(np.arange(len(rt))), 1))
if p0:
    blk._grow(p0)
    for n, dt, w in CN(blk.vpk): getattr(blk, n)[:, :p0] = ld('c_' + n, dt, (4, p0, w) if w > 1 else (4, p0))
if chain:
    nin = 12; hin_dev = ld('hin', f32, (N, H)); src = np.load('/data/yasakani/tmp/ref_h_%d.npy' % (layer - 1))
    cmp('h_in', hin_dev, src[np.arange(N) % nin])                   # layer input on the device (GDN layers below) vs the reference free run
else: src = np.fromfile(tag + 'in.bin', f32).reshape(-1, H); nin = len(src)
out = dict(q16=[], gate=[], y=[], y2=[], h=[]); pos = p0; row = 0
for m in passes:
    st = State(); st.h = src[(row + np.arange(m)) % nin].copy()
    res_norm(st, None, ln1, blk.q.sg_in)
    T = blk.forward(st, m, pos)
    for k in ('q16', 'gate', 'y', 'y2'): out[k].append(blk.dbg[k].reshape(m, 24, 256))
    if chain:
        res_norm(st, T, ln2, mlp.gate.sg_in); T = mlp.forward(st)
        st.h = rbf(st.h + rbf(rot_out(yr(st.acc[T.name], st.us, T.tau), T.sg_out)))
    else: res_norm(st, T, ln2, blk.q.sg_in)
    out['h'].append(st.h); pos += m; row += m
for k in ('q16', 'gate'): cmp(k, ld(k, f32, (N, 24, 256)), np.concatenate(out[k]))
for n, dt, w in CN(blk.vpk): cmp(n, ld(n, dt, (4, pos, w) if w > 1 else (4, pos)), getattr(blk, n)[:, :pos])
for k in ('y', 'y2'): cmp(k, ld(k, f32, (N, 24, 256)), np.concatenate(out[k]))
h = np.concatenate(out['h']); cmp('h', ld('h', f32, (N, H)), h)
if chain and p0 == 0 and N == 12 and layer == 3: cmp('h vs run', ld('h', f32, (N, H)), np.load('/data/yasakani/tmp/ref_h_3.npy'))
print('RESULT %s  (%s layer %d, V %s, pos_start %d, passes %s)' % ('ALL BIT-EXACT' if ok_all else 'MISMATCH', mode, layer, 'packed' if blk.vpk else 'INT8', p0, passes))
