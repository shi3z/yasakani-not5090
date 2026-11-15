// Dumps intermediate buffers of the first steps for comparison with ref/core.py.
#include "../src/engine.hpp"
#include <fstream>
template <class T> static void dump(sycl::queue& q, const char* name, const T* d, size_t n) { q.wait(); std::vector<T> v(n); q.memcpy(v.data(), d, n * sizeof(T)).wait();
  std::ofstream o(std::string("/data/yasakani/tmp/st_") + name + ".bin", std::ios::binary); o.write((char*)v.data(), n * sizeof(T)); }
int main() {
  std::ifstream f("/data/yasakani/tmp/ids.bin", std::ios::binary | std::ios::ate); const int M = (int)f.tellg() / 4; f.seekg(0); std::vector<int> ids(M); f.read((char*)ids.data(), M * 4);
  yx::Engine E("/data/yasakani/model-speed/qwen38_27b_speed.yskn", 1); E.q.memcpy(E.ids, ids.data(), M * 4).wait();
  using namespace yx;
  E.embed(M); dump(E.q, "emb", E.h, (size_t)M * H);
  E.res_norm(nullptr, M, E.L[0].ln1, E.first_sg(0), true);
  dump(E.q, "S", E.S, MRMAX * SST); dump(E.q, "alpha", E.alpha, M * 10); dump(E.q, "qx", E.qx, (size_t)160 * 16 * 32); dump(E.q, "xn", E.xn, (size_t)M * H);
  Gdn& Y = E.L[0].g;
  const char* mode = getenv("YX_MODE") ? getenv("YX_MODE") : "a";
  if (mode[0] == 'a') E.gemm_group({&Y.qkv, &Y.z}, M);
  if (mode[0] == 'b') { E.gemm_group({&Y.qkv, &Y.z}, M); E.gemm_group({&Y.qkv, &Y.z}, M); }
  if (mode[0] == 'c') E.gemm_group({&Y.z, &Y.qkv}, M);
  if (mode[0] == 'd') { E.gemm_group({&Y.qkv}, M); E.q.wait(); E.gemm_group({&Y.z}, M); }
  std::printf("qkv N %d Kc %d bits %d part %p | z N %d part %p (diff %td)\n", Y.qkv.N, Y.qkv.Kc, Y.qkv.bits, (void*)Y.qkv.part, Y.z.N, (void*)Y.z.part, Y.z.part - Y.qkv.part);
  dump(E.q, "us", E.us, M); dump(E.q, "Aw", E.Aw, 16 * 10); dump(E.q, "acc_qkv", Y.qkv.part, (size_t)16 * CV); dump(E.q, "acc_z", Y.z.part, (size_t)16 * ZN);
}
