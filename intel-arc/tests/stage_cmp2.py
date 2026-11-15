import sys, numpy as np
sys.path.insert(0, '.')
from ref.core import *
M = 12
def ld(n, dt, shape): return np.fromfile('work/st_%s.bin' % n, dt).reshape(shape)
qx = ld('qx', np.int8, (160, 16, 32)).transpose(1, 0, 2).reshape(16, H); Aw = ld('Aw', np.int32, (16, 10)).astype(np.int64)
for name, N in (('z', 6144), ('qkv', 10240)):
    T = Tensor('layers.0.linear_attn.in_proj_' + name); w8 = np.asarray(T.w8()).reshape(N, 10, 512).astype(np.int64)
    dots = np.einsum('ngc,mgc->mng', w8, qx.reshape(16, 10, 512).astype(np.int64)); acc = (dots * Aw[:, None, :]).sum(-1)
    got = ld('acc_' + name, np.int64, (16, N))
    print(name, 'kernel == reference on the engine inputs:', bool((got == acc).all()), 'per-row mismatch', (got != acc).mean(1).round(3)[:M])
    if name == 'qkv':
        i = np.argwhere(got != acc)[:3]; print([(int(a), int(b), int(got[a, b]), int(acc[a, b])) for a, b in i])
        d = (got - acc)[0, :8]; print('diff row0', d, 'dots row0 n0', dots[0, 0], 'Aw row0', Aw[0])
