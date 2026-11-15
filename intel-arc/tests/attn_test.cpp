// Full-attention layer test (src/attn.hpp) against ref/core.py; compare with tests/attn_cmp.py.
//   attn_test chain <tag> <layer> <pos_start> <pass rows...>   layers 0..layer on the device from work/ids.bin (layer = 3)
//   attn_test unit  <tag> <layer> <pos_start> <pass rows...>   layer inputs h from <tag>_in.bin (rows used cyclically); h out = after the attention residual
// pos_start > 0: the cache slots [0, pos_start) are loaded from <tag>_c_{kp,kps,vp,vps,v8,vs}.bin.
// env: YX_CTX (cache rows, default 8192), YX_TIME=<reps> (time attn_forward for M = 1 and 8 at the final position), YX_NODUMP=1.
#include "../src/attn.hpp"
#include <chrono>
#include <cstdio>
using namespace yx;
static std::string DIR = "work/attn/";
template <class T> static std::vector<T> rd(const std::string& p) { std::ifstream f(p, std::ios::binary | std::ios::ate); if (!f) throw std::runtime_error("cannot open " + p);
  std::vector<T> v((size_t)f.tellg() / sizeof(T)); f.seekg(0); f.read((char*)v.data(), v.size() * sizeof(T)); return v; }
template <class T> static void wr(const std::string& p, const std::vector<T>& v) { std::ofstream o(p, std::ios::binary); o.write((const char*)v.data(), v.size() * sizeof(T)); }
template <class T> static void app(sycl::queue& q, std::vector<T>& dst, const T* dev, size_t n) { q.wait(); const size_t o = dst.size(); dst.resize(o + n); q.memcpy(dst.data() + o, dev, n * sizeof(T)).wait(); }
static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int main(int argc, char** argv) {
  if (argc < 6) { std::printf("usage: attn_test chain|unit <tag> <layer> <pos_start> <pass rows...>\n"); return 1; }
  const std::string mode = argv[1], tag = DIR + argv[2] + "_"; const int layer = atoi(argv[3]), pos_start = atoi(argv[4]); const bool chain = mode == "chain";
  std::vector<int> passes; for (int i = 5; i < argc; ++i) passes.push_back(atoi(argv[i]));
  const int max_ctx = getenv("YX_CTX") ? atoi(getenv("YX_CTX")) : 8192; const bool nodump = getenv("YX_NODUMP") != nullptr;
  double t0 = now();
  Engine E("models/speed/qwen38_27b_speed.yskn", chain ? layer + 1 : 0);
  auto sh = std::make_shared<AttnShared>(E, max_ctx, "models/speed/kvq_u5.bin");
  Attn A(E, layer, sh); E.q.wait();
  std::printf("loaded in %.1f s; layer %d ordinal %d, V %s, max_ctx %d\n", now() - t0, layer, A.ord, A.vpk ? "packed" : "INT8", max_ctx);
  { std::vector<float> t((size_t)max_ctx * 64); E.q.memcpy(t.data(), sh->rope, t.size() * 4).wait(); wr(tag + "rope.bin", t); }
  std::vector<int> ids; std::vector<float> in; float *ln1 = nullptr, *ln2 = nullptr;
  if (chain) ids = rd<int>("work/ids.bin");
  else { in = rd<float>(tag + "in.bin"); const std::string p = "layers." + std::to_string(layer) + ".";
    ln1 = upload_vec(E.q, E.C, p + "input_layernorm", H); ln2 = upload_vec(E.q, E.C, p + "post_attention_layernorm", H); }
  const int nin = chain ? (int)ids.size() : (int)(in.size() / H);
  if (pos_start > 0) {
    auto ld = [&](const char* n, void* dev, size_t rowbytes) { auto v = rd<uint8_t>(tag + "c_" + n + ".bin"); if (v.size() != 4 * (size_t)pos_start * rowbytes) throw std::runtime_error(std::string("bad cache file ") + n);
      for (int h = 0; h < 4; ++h) E.q.memcpy((uint8_t*)dev + (size_t)h * max_ctx * rowbytes, v.data() + (size_t)h * pos_start * rowbytes, (size_t)pos_start * rowbytes).wait(); };
    ld("kp", A.kp, 160); ld("kps", A.kps, 4);
    if (A.vpk) { ld("vp", A.vp, 160); ld("vps", A.vps, 4); } else { ld("v8", A.v8, 256); ld("vs", A.vs, 4); }
  }
  std::vector<float> d_hin, d_h, d_q16, d_gate, d_y, d_y2;
  int pos = pos_start, row = 0;
  auto feed = [&](int m, int r0) {            // layer input of m rows starting at input row r0 -> E.h, E.qx, E.alpha, E.S
    if (chain) { std::vector<int> pi(m); for (int i = 0; i < m; ++i) pi[i] = ids[(r0 + i) % nin]; E.q.memcpy(E.ids, pi.data(), m * 4).wait();
      E.embed(m); E.res_norm(nullptr, m, E.L[0].ln1, E.first_sg(0), true);
      for (int l = 0; l < layer; ++l) { Layer& Y = E.L[l]; E.gdn(Y.g, m, m); E.res_norm(&Y.g.out, m, Y.ln2, Y.m.gate.sg_in, false);
        E.mlp(Y.m, m); E.res_norm(&Y.m.down, m, E.L[l + 1].ln1, l + 1 == layer ? A.q.sg_in : E.first_sg(l + 1), !E.L[l + 1].full); }
    } else { std::vector<float> hh((size_t)m * H); for (int i = 0; i < m; ++i) std::memcpy(&hh[(size_t)i * H], &in[(size_t)((r0 + i) % nin) * H], H * 4);
      E.q.memcpy(E.h, hh.data(), hh.size() * 4).wait(); E.res_norm(nullptr, m, ln1, A.q.sg_in, false); } };
  for (int m : passes) {
    feed(m, row);
    if (!nodump) app(E.q, d_hin, E.h, (size_t)m * H);
    attn_forward(E, A, m, pos);
    if (!nodump) { app(E.q, d_q16, sh->q16, (size_t)m * A_NO); app(E.q, d_gate, sh->gate, (size_t)m * A_NO); app(E.q, d_y, sh->y, (size_t)m * A_NO); app(E.q, d_y2, sh->y2, (size_t)m * A_NO); }
    if (chain) { Layer& Y = E.L[layer]; E.res_norm(&A.o, m, Y.ln2, Y.m.gate.sg_in, false); E.mlp(Y.m, m); E.res_norm(&Y.m.down, m, E.L[layer + 1].ln1, E.first_sg(layer + 1), true); }
    else E.res_norm(&A.o, m, ln2, A.q.sg_in, false);
    if (!nodump) app(E.q, d_h, E.h, (size_t)m * H);
    pos += m; row += m;
  }
  E.q.wait();
  std::printf("%zu passes, %d rows, final position %d\n", passes.size(), row, pos);
  if (!nodump) {
    wr(tag + "hin.bin", d_hin); wr(tag + "h.bin", d_h); wr(tag + "q16.bin", d_q16); wr(tag + "gate.bin", d_gate); wr(tag + "y.bin", d_y); wr(tag + "y2.bin", d_y2);
    auto dc = [&](const char* n, const void* dev, size_t rowbytes) { std::vector<uint8_t> v(4 * (size_t)pos * rowbytes);
      for (int h = 0; h < 4; ++h) E.q.memcpy(v.data() + (size_t)h * pos * rowbytes, (const uint8_t*)dev + (size_t)h * max_ctx * rowbytes, (size_t)pos * rowbytes).wait(); wr(tag + n + ".bin", v); };
    dc("kp", A.kp, 160); dc("kps", A.kps, 4);
    if (A.vpk) { dc("vp", A.vp, 160); dc("vps", A.vps, 4); } else { dc("v8", A.v8, 256); dc("vs", A.vs, 4); }
  }
  if (getenv("YX_TIME")) { const int reps = atoi(getenv("YX_TIME")); double prof[8];
    for (int m : {1, 8}) { if (pos + m > max_ctx) break; std::vector<double> ts, st(6, 1e9);
      for (int r = 0; r < reps + 3; ++r) { feed(m, 0); E.q.wait(); const double a = now(); attn_forward(E, A, m, pos); E.q.wait(); if (r >= 3) ts.push_back(now() - a); }
      attn_prof = prof;
      for (int r = 0; r < reps; ++r) { feed(m, 0); E.q.wait(); attn_forward(E, A, m, pos); for (int i = 0; i < 6; ++i) st[i] = std::min(st[i], prof[i + 1] - prof[i]); }
      attn_prof = nullptr;
      std::sort(ts.begin(), ts.end());
      std::printf("time attn_forward M=%d pos0=%d: min %.3f ms  median %.3f ms | stages (min, synced): qkv-gemm %.3f post %.3f viterbi %.3f select %.3f attend %.3f combine+o-gemm %.3f ms\n", m, pos, ts[0] * 1e3, ts[ts.size() / 2] * 1e3,
                  st[0] * 1e3, st[1] * 1e3, st[2] * 1e3, st[3] * 1e3, st[4] * 1e3, st[5] * 1e3); }
  }
  return 0;
}
