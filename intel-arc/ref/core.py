# Executable reference of the YASAKANI default path (spec/A_gemv.md, spec/B_layers.md) in numpy float32.
# Slow, CPU only.  Used to validate the spec against the CUDA engine and to unit-test the SYCL kernels.
import os, sys, numpy as np
sys.path.insert(0, '/home/shi3z/yasakani-src/yasakani/tools')
import yskn_decode as Y
f32 = np.float32
K512 = f32(0.044194173824159216)
MD = '/data/yasakani/model-speed'
QUAL = os.environ.get('YX_REF_QUALITY') == '1'
if QUAL: MD = '/data/yasakani/model-quality'
C = Y.Container(MD + ('/qwen38_27b_quality.yskn' if QUAL else '/qwen38_27b_speed.yskn'))
EPS = f32(1e-6)
H = 5120

def rbf(x):
    u = np.ascontiguousarray(x, f32).view(np.uint32).copy()
    u += np.uint32(0x7fff) + ((u >> np.uint32(16)) & np.uint32(1))
    return (u & np.uint32(0xffff0000)).view(f32)
def rf16(x): return x.astype(np.float16).astype(f32)
def fwht(x):                                  # last axis multiple of 512, stages h = 1..256, (a+b, a-b), unnormalised
    s = x.shape; x = np.ascontiguousarray(x, f32).reshape(-1, 512)
    h = 1
    while h < 512:
        y = x.reshape(-1, 512 // (2 * h), 2, h); a = y[:, :, 0, :]; b = y[:, :, 1, :]
        x = np.stack([a + b, a - b], axis=2).reshape(-1, 512); h *= 2
    return x.reshape(s)
def xor_tree32(a):                            # [..., 32] -> [...]
    idx = np.arange(32)
    for o in (16, 8, 4, 2, 1): a = a + a[..., idx ^ o]
    return a[..., 0]
def seq4(w): return ((w[..., 0] + w[..., 1]) + w[..., 2]) + w[..., 3]
def tree128(a): return seq4(xor_tree32(a.reshape(a.shape[:-1] + (4, 32))))      # 128 values: 4 warps of 32
def pmf(b): return np.array(b, np.uint32).view(f32)[()]
def rsqrt(x): return (f32(1) / np.sqrt(np.asarray(x, f32))).astype(f32)        # pm_rsqrt
def _pm_expf1(x):                                                              # pm_math.cuh:18-38, scalar
    x = f32(x); ux = int(np.array(x, f32).view(np.uint32)); ax = ux & 0x7fffffff; neg = ux >> 31
    if ax > 0x7f800000: return x + x
    if not neg and ax >= 0x42b17218: return f32(np.inf)
    if ux > 0xc2cff1b5: return f32(0)
    LN2HI, LN2LO = pmf(0x3f317200), pmf(0x35bfbe8e)
    hi = f32(0); lo = f32(0); r = x; k = 0
    if ax > 0x3eb17218:
        if ax < 0x3f851592:
            hi = f32(x - (-LN2HI if neg else LN2HI)); lo = -LN2LO if neg else LN2LO; k = -1 if neg else 1
        else:
            k = int(f32(f32(pmf(0x3fb8aa3b) * x) + (f32(-0.5) if neg else f32(0.5)))); t = f32(k)
            hi = f32(x - f32(t * LN2HI)); lo = f32(t * LN2LO)
        r = f32(hi - lo)
    elif ax < 0x39000000: return f32(f32(1) + x)
    t = f32(r * r)
    c = f32(r - f32(t * f32(pmf(0x3e2aaa8f) + f32(t * pmf(0xbb355215)))))
    if k == 0: return f32(f32(1) - f32(f32(f32(r * c) / f32(c - f32(2))) - r))
    y = f32(f32(1) - f32(f32(lo - f32(f32(r * c) / f32(f32(2) - c))) - hi))
    if k >= -125:
        if k == 128: return f32(f32(y * f32(2)) * pmf(0x7f000000))
        return f32(y * pmf((0x7f + k) << 23))
    return f32(f32(y * pmf((0x7f + k + 100) << 23)) * pmf(0x0d800000))
_pm_expf = np.vectorize(_pm_expf1, otypes=[f32])
def expf(x): return _pm_expf(np.asarray(x, f32))
def _pm_logf1(x):
    x = f32(x); ix = int(np.array(x, f32).view(np.uint32)); k = 0
    if ix < 0x00800000: x = f32(x * f32(33554432)); k = -25; ix = int(np.array(x, f32).view(np.uint32))
    k += (ix >> 23) - 127; ix &= 0x007fffff
    i = (ix + 0x4afb20) & 0x800000
    m = pmf(ix | (i ^ 0x3f800000)); k += i >> 23
    f = f32(m - f32(1)); s = f32(f / f32(f32(2) + f)); z = f32(s * s); w = f32(z * z)
    t1 = f32(w * f32(pmf(0x3eccce13) + f32(w * pmf(0x3e789e26))))
    t2 = f32(z * f32(pmf(0x3f2aaaaa) + f32(w * pmf(0x3e91e9ee))))
    R = f32(t2 + t1); hfsq = f32(f32(f32(0.5) * f) * f); dk = f32(k)
    return f32(f32(dk * pmf(0x3f317180)) - f32(f32(hfsq - f32(f32(s * f32(hfsq + R)) + f32(dk * pmf(0x3717f7d1)))) - f))
def _pm_softplus1(x):
    x = f32(x)
    if x > f32(20): return x
    y = _pm_expf1(x); u = f32(f32(1) + y)
    c = f32(y - f32(u - f32(1))) if y <= f32(1) else f32(f32(1) - f32(u - y))
    return f32(_pm_logf1(u) + f32(c / u))
softplus = np.vectorize(_pm_softplus1, otypes=[f32])
def silu(x): x = np.asarray(x, f32); return (x / (f32(1) + expf(-x))).astype(f32)
def sigmoid(x): x = np.asarray(x, f32); return (f32(1) / (f32(1) + expf(-x))).astype(f32)
def vec(name): return Y.vector(C, C.vectors[name]).astype(f32)

class Tensor:
    def __init__(self, name):
        t = Y.checked(C.tensors[name]); self.t = t; self.name = name
        self.N, self.Kc, self.bits = t['physical_rows'], t['cols'], t['bits']; self.G = self.Kc // 512
        lut = C.array(t['lut_off'], '<f2', 8192).astype(f32).reshape(4096, 2)
        gamma = f32(np.abs(lut).max() / f32(127))
        self.lut8 = np.clip(np.rint(lut / gamma), -127, 127).astype(np.int8)
        self.tau = (C.array(t['scale_off'], '<f2', self.N).astype(f32) * gamma).astype(f32)
        self.sg_in = Y.sign_stream(t['rot_in']['rule'], t['rot_in']['sign_seed'], t['rot_in']['sign_offset'], self.Kc).astype(f32)
        ro = t['rot_out']
        self.sg_out = None if ro is None else Y.sign_stream(ro['rule'], ro['sign_seed'], ro['sign_offset'], self.N).astype(f32)
        self._w8 = None
    def w8(self):
        if self._w8 is None:
            p = ('/data/yasakani/tmp/w8q/' if QUAL else '/data/yasakani/tmp/w8/') + self.name + '.npy'
            if os.path.exists(p): self._w8 = np.load(p, mmap_mode='r')
            else:
                uw = Y.unit_words(C, self.t); w = np.empty((self.N, self.Kc), np.int8)
                for r0 in range(0, self.N, 2048): w[r0:r0+2048] = self.lut8[Y.states(uw[r0:r0+2048], self.bits)].reshape(-1, self.Kc)
                np.save(p, w); self._w8 = w
        return self._w8

def a8(x, sg_in):
    """x [M][Kc] float (before the sign) -> qx int8 [M][Kc], alpha f32 [M][G]"""
    M, Kc = x.shape
    v = rf16(fwht((x * sg_in[None, :]).astype(f32)) * K512).reshape(M, Kc // 512, 512)
    alpha = (np.abs(v).max(-1) / f32(127)).astype(f32)
    with np.errstate(divide='ignore', invalid='ignore'):
        r = np.where(alpha[..., None] > 0, v / alpha[..., None], f32(0)).astype(f32)
    return np.clip(np.rint(r), -127, 127).astype(np.int8).reshape(M, Kc), alpha
def gemv(T, qx, alpha):
    """-> acc int64 [M][N], us f32 [M]"""
    M = qx.shape[0]
    m = alpha.max(-1); rm = np.where(m > 0, f32(16777216) / m, f32(0)).astype(f32); us = np.ldexp(m, -24).astype(f32)
    A = np.rint((alpha * rm[:, None]).astype(f32)).astype(np.int64)
    w8 = T.w8(); acc = np.empty((M, T.N), np.int64); xq = qx.reshape(M, T.G, 512).astype(np.float64)
    for r0 in range(0, T.N, 1024):                 # float64 dot products are exact here (|dot| < 2^23)
        w = np.asarray(w8[r0:r0+1024]).reshape(-1, T.G, 512).astype(np.float64)
        dots = np.einsum('ngc,mgc->mng', w, xq, optimize=True)
        acc[:, r0:r0+1024] = (dots.astype(np.int64) * A[:, None, :]).sum(-1)
    return acc, us
def yr(acc, us, tau, r=None):
    t = acc.astype(f32) * us[:, None]
    if r is not None: t = t * r[:, None]
    return (t * tau[None, :]).astype(f32)
def rot_out(y, sg_out): return ((fwht(y) * K512) * sg_out[None, :]).astype(f32)
def blocks_ss(h):                                  # S [M][10] per spec A 5.1
    M = h.shape[0]; x = (h * h).astype(f32).reshape(M, -1, 4, 128)      # element t + 128 j
    a = ((x[:, :, 0] + x[:, :, 1]) + x[:, :, 2]) + x[:, :, 3]
    return tree128(a)
def rfac(S):
    s = np.zeros(S.shape[0], f32)
    for b in range(S.shape[1]): s = s + S[:, b]
    return rsqrt(s / f32(5120) + EPS)

def embed(ids):
    t = Y.checked(C.tensors['embed_tokens']); uw = Y.unit_words(C, t)
    lut = C.array(t['lut_off'], '<f2', 8192).astype(f32).reshape(4096, 2); sc = C.array(t['scale_off'], '<f2', t['physical_rows']).astype(f32)
    sg = Y.sign_stream(t['rot_in']['rule'], t['rot_in']['sign_seed'], t['rot_in']['sign_offset'], H).astype(f32)
    out = np.empty((len(ids), H), f32)
    for i, tid in enumerate(ids):
        x = (lut[Y.states(uw[tid:tid+1], t['bits'])].reshape(H) * sc[tid]).astype(f32)
        out[i] = rbf((fwht(x) * K512) * sg)
    return out

class State:                                         # everything a pass carries between kernels
    pass

def res_norm(st, T, w, sg_in, want_xn=False):
    """adds the output of tensor T (st.acc, st.us) to st.h (or keeps the embedding when T is None), prepares the next input"""
    if T is not None: st.h = rbf(st.h + rbf(rot_out(yr(st.acc[T.name], st.us, T.tau), T.sg_out)))
    st.S = blocks_ss(st.h); st.r = rfac(st.S)
    u = (st.h * (f32(1) + w)[None, :]).astype(f32)
    st.xn = rbf(u) if want_xn else None
    st.qx, st.alpha = a8(u, sg_in)
def run_gemv(st, Ts):
    st.acc = {}
    for T in Ts: st.acc[T.name], st.us = gemv(T, st.qx, st.alpha)

class GDN:
    def __init__(self, l):
        p = 'layers.%d.linear_attn.' % l
        self.qkv, self.z, self.out = Tensor(p + 'in_proj_qkv'), Tensor(p + 'in_proj_z'), Tensor(p + 'out_proj')
        self.wa, self.wb = vec(p + 'in_proj_a').reshape(48, H), vec(p + 'in_proj_b').reshape(48, H)
        self.conv = vec(p + 'conv1d').reshape(10240, 4); self.A_log, self.dt_bias, self.gnorm = vec(p + 'A_log'), vec(p + 'dt_bias'), vec(p + 'norm')
        self.S = np.zeros((48, 128, 128), f32); self.hist = np.zeros((3, 10240), f32)
    def ab(self, st):
        M = st.xn.shape[0]; out = np.empty((M, 96), f32)
        W = np.concatenate([self.wb, self.wa])                                   # rows 0..47 b, 48..95 a
        for tok in range(M):
            p = (W * st.xn[tok][None, :]).astype(f32).reshape(96, 5, 128, 8)     # i = 8*(t + 128 j) + e
            acc = np.zeros((96, 128), f32)
            for j in range(5):
                for e in range(8): acc = acc + p[:, j, :, e]
            out[tok] = rbf(tree128(acc) * st.r[tok])
        return out
    def forward(self, st, n_commit):
        M = st.h.shape[0]
        ab = self.ab(st)
        run_gemv(st, [self.qkv, self.z])
        mixed = rbf(rot_out(yr(st.acc[self.qkv.name], st.us, self.qkv.tau, st.r), self.qkv.sg_out))
        z = rbf(rot_out(yr(st.acc[self.z.name], st.us, self.z.tau, st.r), self.z.sg_out))
        xs = np.concatenate([self.hist, mixed])                                  # row r of the pass = xs[r + 3]
        cw = self.conv
        acc = ((cw[None, :, 0] * xs[0:M] + cw[None, :, 1] * xs[1:M+1]) + cw[None, :, 2] * xs[2:M+2]) + cw[None, :, 3] * xs[3:M+3]
        act = rbf(silu(rbf(acc.astype(f32))))
        qk = act[:, :4096].reshape(M, 32, 128)
        y = (qk * rsqrt(tree128((qk * qk).astype(f32)) + f32(1e-6))[..., None]).astype(f32)
        q = (y[:, :16] * f32(0.08838834764831845)).astype(f32); k = y[:, 16:]; v = act[:, 4096:].reshape(M, 48, 128)
        beta = rbf(sigmoid(ab[:, :48])); g = ((-expf(self.A_log))[None, :] * softplus((ab[:, 48:] + self.dt_bias[None, :]).astype(f32))).astype(f32)
        S = self.S.copy(); core = np.empty((M, 48, 128), f32); kh = np.arange(48) // 3
        for t in range(M):
            kt, qt, eg = k[t][kh], q[t][kh], expf(g[t])                           # [48][128], [48]
            kvw = np.zeros((48, 4, 128), f32)
            for i in range(128):
                S[:, i, :] = S[:, i, :] * eg[:, None]; kvw[:, i // 32] = kvw[:, i // 32] + S[:, i, :] * kt[:, i, None]
            d = ((v[t] - seq4(kvw.transpose(0, 2, 1))) * beta[t][:, None]).astype(f32)
            ow = np.zeros((48, 4, 128), f32)
            for i in range(128):
                S[:, i, :] = S[:, i, :] + kt[:, i, None] * d; ow[:, i // 32] = ow[:, i // 32] + S[:, i, :] * qt[:, i, None]
            core[t] = rbf(seq4(ow.transpose(0, 2, 1)))
            if t + 1 == n_commit: self.S = S.copy()
        if n_commit > 0: self.hist = xs[n_commit:n_commit + 3].copy()
        rh = rsqrt(tree128((core * core).astype(f32)) / f32(128) + EPS)
        n2 = rbf(self.gnorm[None, None, :] * rbf(core * rh[..., None]))
        out = rbf(n2.reshape(M, 6144) * silu(z))
        st.qx, st.alpha = a8(out, self.out.sg_in)
        run_gemv(st, [self.out])
        return self.out

class MLP:
    def __init__(self, l):
        p = 'layers.%d.mlp.' % l
        self.gate, self.up, self.down = Tensor(p + 'gate_proj'), Tensor(p + 'up_proj'), Tensor(p + 'down_proj')
    def forward(self, st):
        run_gemv(st, [self.gate, self.up])
        g = rbf(rot_out(yr(st.acc[self.gate.name], st.us, self.gate.tau, st.r), self.gate.sg_out))
        u = rbf(rot_out(yr(st.acc[self.up.name], st.us, self.up.tau, st.r), self.up.sg_out))
        x = rbf(rbf(silu(g)) * u)
        st.qx, st.alpha = a8(x, self.down.sg_in)
        run_gemv(st, [self.down])
        return self.down

# ------------------------------------------------------------------------------------------------------------------
# Full attention (spec/C_attention.md): attn_post, KV5 cache write (rotation, INT8 row, trellis code), packed
# attention (pk3/pk4 semantics: 32-key tiles, splits, combine), output gate, a8 for o_proj.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kv_codec_ref as KV                                     # profile reader + scalar codec (bit-exact vs CUDA probes)
NH, KVH, HD, RD, GQ = 24, 4, 256, 64, 6
LOG2E = f32(1.4426950408889634)
def pm_d(hi, lo=0): return np.array((hi << 32) | lo, np.uint64).view(np.float64)[()]
def _hi(x): return (np.asarray(x, np.float64).view(np.uint64) >> np.uint64(32)).astype(np.int64)
def pm_exp_d(x):                                              # pm_math.cuh pm_exp_d, vectorised; float64 in/out
    x = np.atleast_1d(np.asarray(x, np.float64)); out = np.empty_like(x)
    hx = _hi(x) & 0x7fffffff; xsb = np.signbit(x)
    LN2HI, LN2LO = pm_d(0x3fe62e42, 0xfee00000), pm_d(0x3dea39ef, 0x35793c76)
    with np.errstate(all='ignore'):
        big = hx > 0x3fd62e42; one = big & (hx < 0x3FF0A2B2)
        k = np.where(one, np.where(xsb, -1, 1), np.where(big, np.trunc(pm_d(0x3ff71547, 0x652b82fe) * x + np.where(xsb, -0.5, 0.5)), 0.0))
        k = np.where(np.isfinite(k), k, 0.0)
        hi = np.where(one, x - np.where(xsb, -LN2HI, LN2HI), np.where(big, x - k * LN2HI, 0.0))
        lo = np.where(one, np.where(xsb, -LN2LO, LN2LO), np.where(big, k * LN2LO, 0.0))
        r = np.where(big, hi - lo, x)
        t = r * r
        p = pm_d(0x3FC55555, 0x5555553E) + t * (pm_d(0xBF66C16C, 0x16BEBD93) + t * (pm_d(0x3F11566A, 0xAF25DE2C) + t * (pm_d(0xBEBBBD41, 0xC5D26BF1) + t * pm_d(0x3E663769, 0x72BEA4D0))))
        c = r - t * p
        y0 = 1.0 - ((r * c) / (c - 2.0) - r)
        y = 1.0 - ((lo - (r * c) / (2.0 - c)) - hi)
        ki = k.astype(np.int64)
        yk = np.where(ki >= -1021, np.ldexp(y, ki), np.ldexp(y, ki + 1000) * pm_d(0x01700000))
        out = np.where(ki == 0, y0, yk)
        out = np.where(~big & (hx < 0x3e300000), 1.0 + x, out)
        out = np.where(x > pm_d(0x40862E42, 0xFEFA39EF), np.inf, out)
        out = np.where(x < pm_d(0xc0874910, 0xD52D3051), 0.0, out)
        out = np.where(np.isnan(x), x, out)
    return out
LN2_F = f32(0.6931471805599453)                               # 0x3f317218
def exp2f(x):
    """Float-only deterministic stand-in for CUDA's library exp2f (q35_atxpk4.cuh:340-349,387):
    pm_expf( x * LN2_F ), one f32 multiply then the portable expf.  exp2f(0) == 1 and exp2f(-inf) == 0 exactly, like the
    library.  (Was f32(pm_exp_d(double(x) * ln2)) until the SYCL port: no doubles on the device.)"""
    with np.errstate(over='ignore'): return expf((np.asarray(x, f32) * LN2_F).astype(f32))
def pm_sincos_f(a):                                           # pm_math.cuh pm_sincos_f (double kernels), a >= 0 f32
    x = np.asarray(a, f32).astype(np.float64)
    n = np.trunc(x * pm_d(0x3FE45F30, 0x6DC9C883) + 0.5); ni = n.astype(np.int64)
    r = ((x - n * pm_d(0x3FF921FB, 0x54400000)) - n * pm_d(0x3DD0B461, 0x1A600000)) - n * pm_d(0x3BA3198A, 0x2E037073)
    z = r * r; v = z * r
    R = pm_d(0x3F811111, 0x1110F8A6) + z * (pm_d(0xBF2A01A0, 0x19C161D5) + z * (pm_d(0x3EC71DE3, 0x57B1FE7D) + z * (pm_d(0xBE5AE5E6, 0x8A2B9CEB) + z * pm_d(0x3DE5D93A, 0x5ACFD57C))))
    ks = r + v * (pm_d(0xBFC55555, 0x55555549) + z * R)
    Rc = z * (pm_d(0x3FA55555, 0x5555554C) + z * (pm_d(0xBF56C16C, 0x16C15177) + z * (pm_d(0x3EFA01A0, 0x19CB1590) + z * (pm_d(0xBE927E4F, 0x809C52AD) + z * (pm_d(0x3E21EE9E, 0xBDB4B1C4) + z * pm_d(0xBDA8FAE9, 0xBE8838D4))))))
    ix = _hi(r) & 0x7fffffff; zr = z * Rc
    qx = np.where(ix > 0x3fe90000, 0.28125, ((np.maximum(ix, 0x00200000) - 0x00200000).astype(np.uint64) << np.uint64(32)).view(np.float64))
    kc = np.where(ix < 0x3FD33333, 1.0 - (0.5 * z - zr), (1.0 - qx) - ((0.5 * z - qx) - zr))
    q = ni & 3
    cs = np.where(q == 0, kc, np.where(q == 1, -ks, np.where(q == 2, -kc, ks)))
    sn = np.where(q == 0, ks, np.where(q == 1, kc, np.where(q == 2, -ks, -kc)))
    return cs.astype(f32), sn.astype(f32)
INV_FREQ = np.array([
  0x3f800000, 0x3f1ab32b, 0x3ebaf81b, 0x3e61f835, 0x3e088d77, 0x3da50956, 0x3d47763f, 0x3cf11177, 0x3c91ad39, 0x3c301052, 0x3bd4ca15,
  0x3b80967d, 0x3b1b690d, 0x3abbd3ed, 0x3a6301e2, 0x3a092e02, 0x39a5cb60, 0x394860c1, 0x38f22ce2, 0x3892587e, 0x3830df52, 0x37d5c441,
  0x37812dab, 0x371c1fc4, 0x36bcb0c1, 0x36640cc6, 0x3609cf4a, 0x35a68e4c, 0x35494c57, 0x34f3499d, 0x3493048e, 0x3431af44], np.uint32).view(f32)
def rope_cs(pos):
    """[n] positions -> (cos, sin) [n][32], BF16-rounded.  The CUDA default (q35_rope_table_kernel, pm == 0) calls the
    library cosf/sinf; this is the pm_sincos_f branch of the same kernel (double kernels, rounded to f32)."""
    f = (np.asarray(pos, f32)[:, None] * INV_FREQ[None, :]).astype(f32)
    c, s = pm_sincos_f(f)
    return rbf(c), rbf(s)
def S256(v):                                                  # [..., 256] -> [...]: tree32 per 32 values, then 8 chunks in order
    w = xor_tree32(np.asarray(v, f32).reshape(v.shape[:-1] + (8, 32))); t = np.zeros(w.shape[:-1], f32)
    for i in range(8): t = t + w[..., i]
    return t
_IDX256 = np.arange(256)
def kv_rotate(x, inverse=False):                              # kvx_rotate, last axis 256
    b = np.asarray(x, f32) if inverse else (x * KV.SG).astype(f32)
    for h in (1, 2, 4, 8, 16, 32, 64, 128):
        c = b[..., _IDX256 ^ h]; b = np.where(_IDX256 & h, c - b, b + c).astype(f32)
    y = (b * f32(0.0625)).astype(f32)
    return (y * KV.SG).astype(f32) if inverse else y
def kv_store8(x):                                             # kvx_store: [..., 256] -> int8 [..., 256], f32 scale [...]
    sc = (np.abs(x).max(-1) / f32(127)).astype(f32)
    with np.errstate(divide='ignore', invalid='ignore'):
        q = np.where(sc[..., None] > 0, np.rint((x / sc[..., None]).astype(f32)), f32(0))
    return np.clip(q, -127, 127).astype(np.int8), sc
_GRP = {}
def kv_group(g):
    if g not in _GRP:
        P = KV.group(g)
        P['cdec'] = ((P['ci'].astype(f32) * f32(0.00390625)) * P['gain']).astype(f32)       # encoder codebook (f32)
        P['tab'] = rbf(P['cdec'])                                                         # decode record (BF16), kvqaxpk_build
        P['muf'] = P['mu'] if P['mu'] is not None else np.zeros(256, f32)
        _GRP[g] = P
    return _GRP[g]
def kv_packable(ordl, kind, kb=5):                            # kvqaxpk_packable
    return all(kv_group((ordl * 4 + h) * 2 + kind)[k] == v for h in range(4) for k, v in (('mode', 0), ('kk', kb), ('guard', 0), ('m', 0)))
_U5 = np.arange(256) >> 5
def kv_encode(y, g, pos):
    """Trellis encode of one row (spec C 3.3), all 9 scales x 4 chains at once.  y = q8*sc [256] f32.
    -> None when pos < guard, else (words u32[40], s f32, states[256], x' f32[256])."""
    P = kv_group(g); assert P['mode'] == 0 and P['kk'] == 5
    if pos < P['guard']: return None
    r = np.asarray(y, f32)
    if P['mu'] is not None: r = (r - P['mu']).astype(f32)
    cj = []
    for U in P['U']:
        c = rf16(S256(r * U)); cj.append(c); r = (r - c * U).astype(f32)
    rms = np.sqrt((S256(r * r) / f32(256)).astype(f32)).astype(f32)
    if rms == 0: rms = f32(1)
    s = rf16((rms * KV.MULT).astype(f32)); s = np.where(s == 0, f32(1), s).astype(f32)      # [9]
    xi = np.clip(np.rint(((r[None, :] / s[:, None]).astype(f32) * f32(256)).astype(f32)), -2048, 2048).astype(np.int64).reshape(-1, 64)   # [36][64]
    ci = P['ci']; B = xi.shape[0]; ar = np.arange(B)
    cost = np.full((B, 256), 0x40000000, np.int64); cost[:, 0] = 0; bp = np.empty((64, B, 8), np.int64)
    for t in range(64):
        c2 = cost.reshape(B, 32, 8); jb = c2.argmin(1); mn = np.take_along_axis(c2, jb[:, None, :], 1)[:, 0, :]
        bp[t] = jb; cost = (xi[:, t, None] - ci[None, :]) ** 2 + mn[:, _U5]
    stt = cost.argmin(1); out = np.empty((B, 64), np.int64)
    for t in range(63, -1, -1):
        out[:, t] = stt; stt = (bp[t][ar, stt >> 5] << 3) | (stt >> 5)
    st = out.reshape(KV.NM, 256)
    rec = (P['cdec'][st] * s[:, None]).astype(f32); dd = (r[None, :] - rec).astype(f32)
    e = S256((dd * dd).astype(f32)); mi = int(np.argmin(e))                                 # first minimum
    x = rec[mi]
    for c, U in zip(cj, P['U']): x = (x + c * U).astype(f32)
    if P['mu'] is not None: x = (x + P['mu']).astype(f32)
    bits = ((st[mi][:, None] >> np.arange(4, -1, -1)[None, :]) & 1).astype(np.uint8).reshape(-1)
    words = np.packbits(bits).view('>u4').astype('<u4')
    return words, s[mi], st[mi], x
_DEC_LO = np.maximum(5 * (_IDX256 + 1) - 8, 5 * (_IDX256 & ~63)); _DEC_END = 5 * (_IDX256 + 1)
def kv_states(words):                                         # spec C 3.5 (tba_unpack / pk_state_g): u32 [..., 40] -> states [..., 256]
    bits = np.unpackbits(np.ascontiguousarray(words).astype('>u4').view(np.uint8), axis=-1).astype(np.int64)
    cs = np.concatenate([np.zeros(bits.shape[:-1] + (1,), np.int64), bits], -1)
    st = np.zeros(bits.shape[:-1] + (256,), np.int64)
    for b in range(8):                                         # bit b (from the LSB) of the window [lo, endb)
        p = _DEC_END - 1 - b; st |= np.where(p >= _DEC_LO, bits[..., np.maximum(p, 0)], 0) << b
    return st

class Attn:
    """Full-attention layer l (l % 4 == 3) with its KV5 cache.  forward() writes cache slots pos0..pos0+M-1 and attends."""
    SPLITS, SHORT, LONG = 42, 31, 4096
    def __init__(self, l):
        p = 'layers.%d.self_attn.' % l; self.l = l; self.ord = l // 4
        self.q, self.k, self.v, self.o = Tensor(p + 'q_proj'), Tensor(p + 'k_proj'), Tensor(p + 'v_proj'), Tensor(p + 'o_proj')
        self.qn, self.kn = vec(p + 'q_norm'), vec(p + 'k_norm')                  # stored as weight - 1
        self.gk = [kv_group((self.ord * 4 + h) * 2 + 0) for h in range(4)]; self.gv = [kv_group((self.ord * 4 + h) * 2 + 1) for h in range(4)]
        assert kv_packable(self.ord, 0), 'K stream must be packed (it is in all 16 layers)'
        self.vpk = kv_packable(self.ord, 1)                                      # False for ordinals 10, 14, 15
        self.cap = 0; self._grow(64)
    def _grow(self, n):
        if n <= self.cap: return
        n = max(n, 2 * self.cap)
        def g(name, shape, dt):
            a = np.zeros(shape, dt); old = getattr(self, name, None)
            if old is not None: a[:, :old.shape[1]] = old
            setattr(self, name, a)
        g('kp', (4, n, 40), np.uint32); g('kps', (4, n), f32)                    # packed K payload + scale
        if self.vpk: g('vp', (4, n, 40), np.uint32); g('vps', (4, n), f32)       # packed V
        else: g('v8', (4, n, 256), np.int8); g('vs', (4, n), f32)                # INT8 V = store8(x')
        self.cap = n
    def _norm_rope(self, y, w, cs):
        """y [M][heads][256] (rbf'd projection output) -> RMS norm with 1+w, RoPE on dims 0..63.  rsqrtf: CUDA default
        uses the library rsqrtf (q35_rsqrt), here pm_rsqrt = 1/sqrt."""
        r = rsqrt(S256((y * y).astype(f32)) / f32(256) + EPS)
        xn = rbf((y * r[..., None]).astype(f32) * (f32(1) + w)[None, None, :])
        c, s = cs[0][:, None, :], cs[1][:, None, :]; a, b = xn[..., :32], xn[..., 32:64]
        out = xn.copy()
        out[..., :32] = rbf(rbf(a * c) + rbf((-b) * s)); out[..., 32:64] = rbf(rbf(b * c) + rbf(a * s))
        return out
    def _write(self, kr, vr, pos0):
        """kr, vr [M][4][256]: rotated K / V rows before the INT8 step.  Stores slots pos0.."""
        M = kr.shape[0]; self._grow(pos0 + M)
        k8, ks = kv_store8(kr); v8, vs = kv_store8(vr)
        for tok in range(M):
            pos = pos0 + tok
            for h in range(4):
                w, s, stt, _ = kv_encode((k8[tok, h].astype(f32) * ks[tok, h]).astype(f32), (self.ord * 4 + h) * 2, pos)
                self.kp[h, pos] = w; self.kps[h, pos] = s
                yv = (v8[tok, h].astype(f32) * vs[tok, h]).astype(f32)
                E = kv_encode(yv, (self.ord * 4 + h) * 2 + 1, pos)
                if self.vpk: self.vp[h, pos] = E[0]; self.vps[h, pos] = E[1]
                else: self.v8[h, pos], self.vs[h, pos] = kv_store8(yv if E is None else E[3])   # spec C 3.6 (guard row: store8(y))
    def _keys(self, h, sl):                                    # decode as the attention does: BF16 table values, scale separate
        return self.gk[h]['tab'][kv_states(self.kp[h, sl])], self.kps[h, sl]
    def _vals(self, h, sl):
        if self.vpk: return self.gv[h]['tab'][kv_states(self.vp[h, sl])], self.vps[h, sl]
        return self.v8[h, sl].astype(f32), self.vs[h, sl]
    @staticmethod
    def _dots(q, K):
        """q [6][256], K [n][256] (both BF16 values) -> [6][n].  Prefix-key dot product in a FIXED f32 order (CUDA: 16
        mma.m16n8k16 steps whose internal order is unspecified; the port cannot use doubles).  Like the mma form it takes
        the 16-element chunks c = 0..15 in order; the 16 products of a chunk (each exact in f32) are reduced by the XOR tree
        for o in (8, 4, 2, 1): a[i] = a[i] + a[i ^ o]  -> a[0],  then  acc = acc + a[0]."""
        pr = (q[:, None, :] * K[None, :, :]).astype(f32).reshape(q.shape[0], K.shape[0], 16, 16)      # [.., c, i]
        idx = np.arange(16); acc = np.zeros(pr.shape[:2], f32)
        for c in range(16):
            a = pr[:, :, c, :]
            for o in (8, 4, 2, 1): a = a + a[..., idx ^ o]
            acc = acc + a[..., 0]
        return acc
    @staticmethod
    def _dot_own(q, K):                                        # q [6][256], K [256] -> [6]; fixed f32 order (q35_atxpk4.cuh:367-383)
        pr = (q * K[None, :]).astype(f32).reshape(-1, 16, 2, 4, 2)                # e = 16kq + 8a + 2r + b
        p = np.zeros((q.shape[0], 4), f32)
        for kq in range(16):
            for a in range(2):
                for b in range(2): p = p + pr[:, kq, a, :, b]
        return (p[:, 0] + p[:, 1]) + (p[:, 2] + p[:, 3])
    def _partial(self, q, h, ln, own, lo, ph, has_self):
        """one split of one row, 6 heads of KV head h: -> m [6], l [6], o [6][256] (spec C 4.4)"""
        m = np.full(GQ, f32(-1e30), f32); l = np.zeros(GQ, f32); o = np.zeros((GQ, 256), f32)
        for t0 in range((lo // 32) * 32, ph, 32):               # tiles of 32 slots on absolute boundaries
            a, b = max(t0, lo), min(t0 + 32, ph)
            if b <= a: continue
            Kv, ksc = self._keys(h, slice(a, b)); Vv, vsc = self._vals(h, slice(a, b))
            sc = np.full((GQ, 32), -np.inf, f32); sc[:, a - t0:b - t0] = self._dots(q, Kv) * (ksc * f32(0.0625)).astype(f32)[None, :]
            x = np.maximum(m, sc.max(1)); c = exp2f((m - x) * LOG2E); m = x; y0 = (x * LOG2E).astype(f32)
            p = exp2f((sc * LOG2E).astype(f32) - y0[:, None])                     # masked keys: exp2f(-inf) = 0
            pr = (p[:, 0::2] + p[:, 1::2]).reshape(GQ, 4, 4)                      # [jj][r]: p[8jj+2r] + p[8jj+2r+1]
            t = ((pr[:, 0] + pr[:, 1]) + pr[:, 2]) + pr[:, 3]
            l = (l * c + ((t[:, 0] + t[:, 1]) + (t[:, 2] + t[:, 3]))).astype(f32)
            o = (o * c[:, None]).astype(f32)
            pf = rbf((p[:, a - t0:b - t0] * vsc[None, :]).astype(f32))            # BF16 rounding of p*vs
            for j in range(b - a):                                                # FIXED f32 order (CUDA: two mma steps of 16 keys,
                o = (o + pf[:, j, None] * Vv[j][None, :]).astype(f32)             # order unspecified): the tile's keys in slot order
        if has_self:
            Kv, ksc = self._keys(h, slice(own, own + 1)); Vv, vsc = self._vals(h, slice(own, own + 1))
            sc = (self._dot_own(q, Kv[0]) * f32(ksc[0] * f32(0.0625))).astype(f32)
            mn = np.maximum(m, sc); c = exp2f((m - mn) * LOG2E); p = exp2f((sc - mn) * LOG2E); m = mn; l = (l * c + p).astype(f32)
            pv = (p * vsc[0]).astype(f32)
            o = ((o * c[:, None]).astype(f32) + (pv[:, None] * Vv[0][None, :]).astype(f32)).astype(f32)
        if self.vpk: o = (o + (l[:, None] * self.gv[h]['muf'][None, :]).astype(f32)).astype(f32)
        return m, l, o
    def attend(self, q16, pos0, depth=None):
        """q16 [M][24][256] (rotated queries, BF16 values) -> y [M][24][256] in the rotated domain (spec C 4.1-4.5)"""
        M = q16.shape[0]; y = np.empty((M, NH, 256), f32)
        for tk in range(M):
            dp = tk if depth is None else int(depth[tk]); ln = pos0 + dp + 1; own = pos0 + tk
            S = self.SHORT if ln < self.LONG else self.SPLITS; per = (ln + S - 1) // S
            for h in range(4):
                q = q16[tk, h * GQ:(h + 1) * GQ]
                pm = np.full((S, GQ), f32(-1e30), f32); pl = np.zeros((S, GQ), f32); pa = np.zeros((S, GQ, 256), f32)
                for s in range(S):
                    lo = s * per; hi = min(ln, lo + per); ph = max(lo, min(hi, ln - 1)); has_self = lo <= ln - 1 < lo + per
                    if ph > lo or has_self: pm[s], pl[s], pa[s] = self._partial(q, h, ln, own, lo, ph, has_self)
                M2 = np.maximum(f32(-1e30), pm.max(0))                            # combine (q35_combine_kernel); expf: library in CUDA
                with np.errstate(over='ignore'): w = np.where(pm > f32(-1e29), expf(np.where(pm > f32(-1e29), pm - M2[None, :], f32(0))), f32(0)).astype(f32)
                L = np.zeros(GQ, f32); O = np.zeros((GQ, 256), f32)
                for s in range(S): L = L + w[s] * pl[s]; O = O + w[s][:, None] * pa[s]
                y[tk, h * GQ:(h + 1) * GQ] = O / L[:, None]
        return y
    def forward(self, st, n_commit, pos0, depth=None):
        """n_commit is not needed: rows are written at their final slots, a later pass overwrites rejected ones (spec C 5.4).
        depth: optional wk table (tree passes); None = chain."""
        M = st.qx.shape[0]; self.M = M
        run_gemv(st, [self.q, self.k, self.v])
        f = lambda T: rbf(rot_out(yr(st.acc[T.name], st.us, T.tau, st.r), T.sg_out))
        yq = f(self.q).reshape(M, NH, 512); yk = f(self.k).reshape(M, KVH, 256); yv = f(self.v).reshape(M, KVH, 256)
        gate = yq[:, :, 256:]                                                     # gate pre-activation = upper half of each 512 block
        cs = rope_cs(pos0 + (np.arange(M) if depth is None else np.asarray(depth)))
        q16 = rbf(kv_rotate(self._norm_rope(yq[:, :, :256], self.qn, cs)))
        self._write(kv_rotate(self._norm_rope(yk, self.kn, cs)), kv_rotate(yv), pos0)
        y = rbf(kv_rotate(self.attend(q16, pos0, depth), True))
        y2 = rbf(y * rbf(sigmoid(gate)))                                          # sigmoid: 1/(1+expf(-x)), library expf in CUDA
        self.dbg = dict(q16=q16, gate=gate, y=y, y2=y2)
        st.qx, st.alpha = a8(y2.reshape(M, NH * 256), self.o.sg_in)
        run_gemv(st, [self.o])
        return self.o
