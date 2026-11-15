import sys, numpy as np
sys.path.insert(0, '/home/shi3z/yasakani-src/yasakani/tools')
import yskn_decode as Y
c = Y.Container('/home/shi3z/yasakani-src/model-speed/qwen38_27b_speed.yskn')
M = 8
for name in sys.argv[1:]:
    t = Y.checked(c.tensors[name]); rows, cols, bits = t['physical_rows'], t['cols'], t['bits']; S = cols // 256
    lutf = c.array(t['lut_off'], '<f2', 8192).astype(np.float32).reshape(4096, 2)
    q = np.clip(np.rint(lutf / (np.abs(lutf).max() / np.float32(127))), -127, 127).astype(np.int8)
    words = np.array(c.array(t['stream_off'], '<u4', rows * S * 4 * bits))
    x = np.random.default_rng(1).integers(-127, 128, (M, cols)).astype(np.int8)
    xa = np.ascontiguousarray(x.reshape(M, S * 8, 32).transpose(1, 0, 2))            # [chunk][m][32]
    ref = np.empty((M, rows), np.int32)
    uw = Y.unit_words(c, t)
    for r0 in range(0, rows, 1024):
        ref[:, r0:r0+1024] = (q[Y.states(uw[r0:r0+1024], bits)].reshape(-1, cols).astype(np.int32) @ x.T.astype(np.int32)).T
    p = '/data/yasakani/tmp/' + name
    np.array([rows, cols, bits, M], np.int32).tofile(p + '.hdr'); words.tofile(p + '.words')
    q.view(np.uint16).reshape(-1).astype(np.uint32).tofile(p + '.lut'); xa.tofile(p + '.x'); ref.tofile(p + '.ref')
    print(name, rows, cols)
