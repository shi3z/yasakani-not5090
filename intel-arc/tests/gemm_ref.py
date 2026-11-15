# Reference for spec/A_gemv.md section 8 on a real tensor: writes inputs and the exact int64 result.
import sys, numpy as np
sys.path.insert(0, '../tools')
import yskn_decode as Y
c = Y.Container('models/speed/qwen38_27b_speed.yskn')
name, MR = sys.argv[1], int(sys.argv[2])
t = Y.checked(c.tensors[name]); rows, cols, bits = t['physical_rows'], t['cols'], t['bits']; S = cols // 256; G = cols // 512
lutf = c.array(t['lut_off'], '<f2', 8192).astype(np.float32).reshape(4096, 2)
gamma = np.float32(np.abs(lutf).max() / np.float32(127))
q = np.clip(np.rint(lutf / gamma), -127, 127).astype(np.int8)
words = np.array(c.array(t['stream_off'], '<u4', rows * S * 4 * bits))
rng = np.random.default_rng(3)
qx = rng.integers(-127, 128, (MR, cols)).astype(np.int8)
Aw = rng.integers(1, 1 << 24, (MR, G)).astype(np.int32); Aw[:, 0] = 1 << 24
uw = Y.unit_words(c, t)
ref = np.zeros((MR, rows), np.int64)
for r0 in range(0, rows, 512):
    w8 = q[Y.states(uw[r0:r0+512], bits)].reshape(-1, G, 512).astype(np.int64)
    dots = np.einsum('ngc,mgc->mng', w8, qx.reshape(MR, G, 512).astype(np.int64))
    ref[:, r0:r0+512] = (dots * Aw[:, None, :].astype(np.int64)).sum(-1)
p = 'work/t_' + name
np.array([rows, cols, bits, MR], np.int32).tofile(p + '.hdr'); words.tofile(p + '.words')
q.view(np.uint16).reshape(-1).astype(np.uint32).tofile(p + '.lut')
np.ascontiguousarray(qx.reshape(MR, cols // 32, 32).transpose(1, 0, 2)).tofile(p + '.qx'); Aw.tofile(p + '.aw'); ref.tofile(p + '.ref')
print(name, rows, cols, bits, MR)
