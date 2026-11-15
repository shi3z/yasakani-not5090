// Dumps everything the GDN layers + MLP produce (rows < M only) over three passes that share the recurrent state:
//   pass 1: M1 rows, all committed;  pass 2: M2 rows, nc2 committed;  pass 3: M1 rows, all committed.
// Run once with YX_SCALAR=1 and once without, then `cmp` the two files:   fast_cmp <layers> <M1> <M2> <nc2> <out>
#include "../src/engine.hpp"
#include <cstdio>
#include <fstream>
int main(int argc, char** argv) {
  const int upto = atoi(argv[1]), M1 = atoi(argv[2]), M2 = atoi(argv[3]), nc2 = atoi(argv[4]);
  std::ifstream f("work/ids.bin", std::ios::binary); std::vector<int> id12(12); f.read((char*)id12.data(), 48);
  yx::Engine E("models/speed/qwen38_27b_speed.yskn", upto); using namespace yx;
  std::ofstream o(argv[5], std::ios::binary);
  auto dump = [&](const void* p, size_t bytes) { std::vector<char> b(bytes); E.q.wait(); E.q.memcpy(b.data(), p, bytes).wait(); o.write(b.data(), bytes); };
  auto pass = [&](int M, int nc, int shift) {
    std::vector<int> ids(M); for (int i = 0; i < M; ++i) ids[i] = id12[(i + shift) % 12]; E.q.memcpy(E.ids, ids.data(), M * 4).wait();
    E.embed(M); dump(E.h, (size_t)M * H * 4);
    E.res_norm(nullptr, M, E.L[0].ln1, E.first_sg(0), true);
    for (int l = 0; l < upto; ++l) { Layer& Y = E.L[l];
      dump(E.S, M * SST * 4); dump(E.xn, (size_t)M * H * 2);
      E.gdn(Y.g, M, nc);
      dump(E.us, M * 4); dump(E.ab, M * 96 * 4); dump(E.z, (size_t)M * ZN * 4); dump(E.core, (size_t)M * ZN * 4); dump(Y.g.mixed, (size_t)M * CV * 2);
      dump(Y.g.q, M * 2048 * 4); dump(Y.g.k, M * 2048 * 4); dump(Y.g.v, (size_t)M * ZN * 4); dump(Y.g.beta, M * 48 * 4); dump(Y.g.g, M * 48 * 4);
      dump(Y.g.S, 48 * 128 * 128 * 4); dump(Y.g.hist, 3 * CV * 4);
      E.res_norm(&Y.g.out, M, Y.ln2, Y.m.gate.sg_in, false); dump(E.h, (size_t)M * H * 4); dump(E.S, M * SST * 4); dump(E.alpha, M * 10 * 4);
      E.mlp(Y.m, M); dump(E.us, M * 4);
      E.res_norm(&Y.m.down, M, E.L[l + 1].ln1, E.first_sg(l + 1), !E.L[l + 1].full); dump(E.h, (size_t)M * H * 4); dump(E.alpha, M * 10 * 4);
      { const int MR = Engine::mr_of(M); std::vector<int8_t> qx((size_t)MRMAX * F); E.q.wait(); E.q.memcpy(qx.data(), E.qx, qx.size()).wait();
        for (int c = 0; c < H / 32; ++c) o.write((char*)&qx[(size_t)c * MR * 32], M * 32); } } };
  pass(M1, M1, 0); pass(M2, nc2, 3); pass(M1, M1, 5);
  std::printf("wrote %s (%ld bytes)\n", argv[5], (long)o.tellp());
}
