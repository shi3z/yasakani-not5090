# Reference data from the CUDA engine (RTX 5060 Ti): hidden states after chosen layers, logits, greedy continuation.
import ctypes as C, sys, json, numpy as np
from tokenizers import Tokenizer
MD = '/data/yasakani/model-speed'; OUT = '/data/yasakani/tmp/oracle/'
import os
os.environ.update({"YSKN_BDR_SHEAD_M": "512", "YSKN_Q35_G6": "1", "YSKN_Q35_KV8": "1", "YSKN_Q35_KV_FP16": "0", "YSKN_Q35_LAYER_OVERLAP": "1",
  "YSKN_Q35_LAYER_ROWS": "4096", "YSKN_Q35_PFALT": "1", "YSKN_Q35_PMAX": "1024", "YSKN_Q35_SHARED_WEIGHTS": "1", "YSKN_Q35_ATX": "1", "YSKN_TB_CAP": "1",
  "YSKN_TB_VIT2": "1", "YSKN_ATX_PK3_PLUT": "1", "YSKN_ATX_PKV": "3", "YSKN_ATX_VSP": "7",
  "YSKN_BDR_SHEAD": MD + "/bdraft2_k8.shead", "YSKN_TB_PROFILE": MD + "/kvq_u5.bin"})
os.environ.update(dict(kv.split('=') for kv in os.environ.get('ORACLE_ENV', '').split(',') if kv))
lib = C.CDLL('/home/shi3z/yasakani-src/yasakani/build/libyskn.so')
lib.yskn_open.restype = C.c_void_p; lib.yskn_open.argtypes = [C.c_char_p, C.c_int, C.c_int, C.c_int]
lib.yskn_last_error.restype = C.c_char_p
P = C.c_void_p
def ck(r):
    if r != 0: raise RuntimeError(lib.yskn_last_error().decode())
layers = [int(x) for x in sys.argv[1].split(',')] if len(sys.argv) > 1 else list(range(8))
tag = sys.argv[2] if len(sys.argv) > 2 else 'a'
text = sys.argv[3] if len(sys.argv) > 3 else 'The capital of France is Paris. The capital of Japan is'
ids = np.array(Tokenizer.from_file(MD + '/tokenizer.json').encode(text, add_special_tokens=False).ids, np.int32)
e = lib.yskn_open((MD + '/qwen38_27b_speed.yskn').encode(), 2048, 0, 0)
if not e: raise RuntimeError(lib.yskn_last_error().decode())
la = np.array(layers, np.int32)
ck(lib.yskn_set_capture(P(e), la.ctypes.data_as(P), len(layers)))
first = C.c_int(0)
ck(lib.yskn_prefill(P(e), ids.ctypes.data_as(P), len(ids), C.byref(first)))
n = len(ids); H = 5120
caps = np.empty((len(layers), n, H), np.float32)
for k in range(len(layers)): ck(lib.yskn_capture_rows(P(e), k, n, caps[k].ctypes.data_as(P)))
logits = np.empty(248320, np.float32); ck(lib.yskn_logits(P(e), logits.ctypes.data_as(P)))
nxt = np.empty(16, np.int32); ck(lib.yskn_decode(P(e), nxt.ctypes.data_as(P), 16))
np.savez(OUT + tag + '.npz', ids=ids, layers=la, caps=caps, logits=logits, first=first.value, next=nxt)
print('ids', ids.tolist()); print('first', first.value, 'argmax', int(logits.argmax()), 'next', nxt.tolist())
print('caps norms', np.linalg.norm(caps[:, -1], axis=1)[:8])
lib.yskn_close(P(e))
