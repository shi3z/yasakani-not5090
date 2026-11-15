import sys, numpy as np
sys.path.insert(0, '.')
from ref.core import *
ids = np.fromfile('work/ids.bin', np.int32); M = len(ids)
def ld(n, dt, shape): return np.fromfile('work/st_%s.bin' % n, dt).reshape(shape)
st = State(); st.h = embed(ids); qkv = Tensor('layers.0.linear_attn.in_proj_qkv')
res_norm(st, None, vec('layers.0.input_layernorm'), qkv.sg_in, want_xn=True)
al = ld('alpha', f32, (M, 10)); i = np.argwhere(al != st.alpha)[0]; tok, b = int(i[0]), int(i[1])
print('block', tok, b, 'device alpha', al[tok, b].view(np.uint32), 'ref', st.alpha[tok, b].view(np.uint32), float(al[tok, b]), float(st.alpha[tok, b]))
u = (st.h * (f32(1) + vec('layers.0.input_layernorm'))[None, :]).astype(f32)
v = rf16(fwht((u * qkv.sg_in[None, :]).astype(f32)) * K512).reshape(M, 10, 512)[tok, b]
amax = np.abs(v).max(); print('amax', float(amax), amax.view(np.uint32), 'amax/127 f32', (amax / f32(127)).view(np.uint32), 'via double', f32(np.float64(amax) / 127.0).view(np.uint32))
print('device alpha*127', float(al[tok, b]) * 127, 'is amax representable in f16:', float(np.float16(amax)) == float(amax))
pre = (fwht((u * qkv.sg_in[None, :]).astype(f32)) * K512).reshape(M, 10, 512)[tok, b]; j = np.abs(v).argmax(); print('pre-round value at argmax', float(pre[j]), pre[j].view(np.uint32), '-> f16', float(v[j]), ' second largest', float(np.sort(np.abs(v))[-2]))
