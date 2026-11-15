// SPDX-License-Identifier: Apache-2.0
// .yskn container reader and device upload (docs/FORMAT.md of YASAKANI; spec/A_gemv.md section 2).
#pragma once
#include <sycl/sycl.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include "json.hpp"
namespace yx {
using json = nlohmann::json;
inline float h2f_host(uint16_t h) {
  const uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023; uint32_t u;
  if (e == 0) { if (!m) u = s; else { int sh = 0; uint32_t mm = m; while (!(mm & 1024)) { mm <<= 1; ++sh; } u = s | ((uint32_t)(113 - sh) << 23) | ((mm & 1023) << 13); } }
  else if (e == 31) u = s | 0x7f800000u | (m << 13);
  else u = s | ((e + 112) << 23) | (m << 13);
  float f; std::memcpy(&f, &u, 4); return f;
}
inline std::vector<int8_t> sign_stream(uint64_t seed, size_t offset, size_t n) {      // rule mulberry32_7919_13
  std::vector<int8_t> out(n); uint32_t s = (uint32_t)seed * 7919u + 13u; if (!s) s = 0x9e3779b9u;
  for (size_t k = 0; k < offset + n; ++k) {
    uint32_t t = (s += 0x6d2b79f5u); t = (t ^ (t >> 15)) * (t | 1u); t ^= t + (t ^ (t >> 7)) * (t | 61u); t ^= t >> 14;
    if (k >= offset) out[k - offset] = t < 0x80000000u ? -1 : 1;
  }
  return out;
}
struct Container {
  int fd = -1; const uint8_t* base = nullptr; size_t size = 0; json hdr;
  void open(const std::string& path) {
    fd = ::open(path.c_str(), O_RDONLY); if (fd < 0) throw std::runtime_error("cannot open " + path);
    struct stat st; fstat(fd, &st); size = st.st_size;
    void* p = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0); if (p == MAP_FAILED) throw std::runtime_error("mmap failed: " + path);
    base = (const uint8_t*)p;
    if (std::memcmp(base, "YSKN", 4)) throw std::runtime_error("not a YSKN file: " + path);
    uint64_t jl; std::memcpy(&jl, base + 8, 8);
    hdr = json::parse(base + 32, base + 32 + jl);
  }
  const json& tensor(const std::string& name) const { for (auto& t : hdr["tensors"]) if (t["name"] == name) return t; throw std::runtime_error("no tensor " + name); }
  const json& vector(const std::string& name) const { for (auto& v : hdr["vectors"]) if (v["name"] == name) return v; throw std::runtime_error("no vector " + name); }
  bool has_tensor(const std::string& name) const { for (auto& t : hdr["tensors"]) if (t["name"] == name) return true; return false; }
  ~Container() { if (base) munmap((void*)base, size); if (fd >= 0) ::close(fd); }
};
// A compressed matrix on the device.
struct Tensor {
  std::string name; int N = 0, Kc = 0, G = 0, bits = 0; uint64_t lut_id = 0;   // lut_id: hash of the value table (matrices with the same table can share a launch)
  uint32_t* stream = nullptr; uint32_t* lut = nullptr; float* tau = nullptr; float* sg_in = nullptr; float* sg_out = nullptr;
  int64_t* part = nullptr;        // [MRmax][N], assigned by the engine
};
template <class T> inline T* to_dev(sycl::queue& q, const T* src, size_t n) {
  T* d = sycl::malloc_device<T>(n, q); if (!d) throw std::runtime_error("out of device memory");
  q.memcpy(d, src, n * sizeof(T)).wait(); return d;      // synchronous: callers pass temporaries
}
inline Tensor upload(sycl::queue& q, const Container& C, const std::string& name) {
  const json& t = C.tensor(name); Tensor T; T.name = name;
  T.N = t.value("physical_rows", t["rows"].get<int>()); T.Kc = t["cols"]; T.G = T.Kc / 512; T.bits = t["bits"];
  if (t.value("layout", std::string("warp")) != "warp" || T.N % 32 || T.Kc % 512) throw std::runtime_error("unsupported tensor " + name);
  const size_t words = (size_t)T.N * (T.Kc / 256) * 4 * T.bits;
  T.stream = to_dev(q, (const uint32_t*)(C.base + t["stream_off"].get<uint64_t>()), words);
  const uint16_t* l16 = (const uint16_t*)(C.base + t["lut_off"].get<uint64_t>());
  float mx = 0.f; for (int i = 0; i < 8192; ++i) mx = std::max(mx, std::fabs(h2f_host(l16[i])));
  const float gamma = mx / 127.f; std::vector<uint32_t> lut(4096);
  for (int s = 0; s < 4096; ++s) { int q0 = (int)std::nearbyint(h2f_host(l16[2 * s]) / gamma), q1 = (int)std::nearbyint(h2f_host(l16[2 * s + 1]) / gamma);
    q0 = std::max(-127, std::min(127, q0)); q1 = std::max(-127, std::min(127, q1)); lut[s] = (uint32_t)(uint8_t)(int8_t)q0 | ((uint32_t)(uint8_t)(int8_t)q1 << 8); }
  T.lut = to_dev(q, lut.data(), 4096);
  { uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)T.bits; for (uint32_t v : lut) { h ^= v; h *= 0x100000001b3ull; } T.lut_id = h; }
  const uint16_t* s16 = (const uint16_t*)(C.base + t["scale_off"].get<uint64_t>());
  std::vector<float> tau(T.N); for (int r = 0; r < T.N; ++r) tau[r] = h2f_host(s16[r]) * gamma;
  T.tau = to_dev(q, tau.data(), tau.size());
  auto sgf = [&](const json& rot, size_t n) { auto s = sign_stream(rot["sign_seed"].get<uint64_t>(), rot["sign_offset"].get<uint64_t>(), n);
    std::vector<float> f(s.begin(), s.end()); return to_dev(q, f.data(), n); };
  T.sg_in = sgf(t["rot_in"], T.Kc);
  if (t.contains("rot_out") && !t["rot_out"].is_null() && t["rot_out"].value("kind", std::string("H512")) != "none") T.sg_out = sgf(t["rot_out"], T.N);
  return T;
}
// bf16 vector -> float on the device
inline float* upload_vec(sycl::queue& q, const Container& C, const std::string& name, size_t n) {
  const json& v = C.vector(name); if (v["n"].get<size_t>() != n || v.value("dtype", std::string("f16")) != "bf16") throw std::runtime_error("bad vector " + name);
  const uint16_t* p = (const uint16_t*)(C.base + v["off"].get<uint64_t>()); std::vector<float> f(n);
  for (size_t i = 0; i < n; ++i) { uint32_t u = (uint32_t)p[i] << 16; std::memcpy(&f[i], &u, 4); }
  return to_dev(q, f.data(), n);
}
}  // namespace yx
