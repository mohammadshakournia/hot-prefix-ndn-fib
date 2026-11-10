// profile_v3.cpp -- inputs for the Stage D cost model.
//
// For one FIB + request stream + cache capacity it measures, with per-lookup
// lfence-serialized TSC timing (calibrated overhead subtracted):
//   hash pass  : the plain hash FIB on every request
//   cache pass : Hot-prefix (HP-A, sampled tracking 1/16, no cutoff) on the same requests,
//                recording which lookups were cache hits
// and reports
//   h       hit ratio of the cache pass (deterministic)
//   F_hot   mean hash-FIB cost over the requests that the cache answered (hit set)
//   F_cold  mean hash-FIB cost over the other requests
//   T_hash  mean hash-FIB cost
//   c_hit   mean cost of a cache hit
//   c_miss  mean cost of a cache miss
//   T_cache mean cache cost
// Each repetition runs hash, cache, hash (bracketing) on fresh engines after an
// untimed warm-up pass; the two hash passes are averaged.
//
// Usage: profile_v3 FIB.csv QUERIES.csv CAPACITY REPS OUT.csv TAG
#include "fib_v3.hpp"

#include <x86intrin.h>

#include <fstream>
#include <iomanip>
#include <iostream>

using namespace fibv3;

static double g_ns = 0, g_over = 0;

static void calibrate() {
  auto c0 = std::chrono::steady_clock::now();
  const uint64_t t0 = __rdtsc();
  while (std::chrono::steady_clock::now() - c0 < std::chrono::milliseconds(200)) {}
  const uint64_t t1 = __rdtsc();
  g_ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - c0).count() / double(t1 - t0);
  std::vector<uint64_t> o(20000);
  for (auto& x : o) {
    _mm_lfence(); const uint64_t a = __rdtsc(); _mm_lfence();
    _mm_lfence(); const uint64_t b = __rdtsc(); _mm_lfence();
    x = b - a;
  }
  std::nth_element(o.begin(), o.begin() + o.size() / 2, o.end());
  g_over = double(o[o.size() / 2]);
}

template <class F>
static void timed(size_t n, F&& f, std::vector<float>& ns) {
  ns.resize(n);
  for (size_t i = 0; i < n; ++i) {
    _mm_lfence(); const uint64_t a = __rdtsc(); _mm_lfence();
    f(i);
    _mm_lfence(); const uint64_t b = __rdtsc(); _mm_lfence();
    ns[i] = static_cast<float>(std::max(0.0, (double(b - a) - g_over) * g_ns));
  }
}

int main(int argc, char** argv) {
  if (argc < 7) { std::cerr << "usage: profile_v3 FIB QUERIES C REPS OUT TAG\n"; return 1; }
  calibrate();
  std::ifstream f(argv[1]), g(argv[2]);
  std::string line;
  std::vector<std::pair<std::string, int>> E;
  std::getline(f, line);
  std::unordered_set<std::string> seen;
  while (std::getline(f, line)) {
    const auto c = line.rfind(',');
    std::string p = canonicalize(line.substr(0, c));
    if (!p.empty() && seen.insert(p).second) E.emplace_back(std::move(p), std::stoi(line.substr(c + 1)));
  }
  std::string buf;
  std::vector<std::pair<size_t, size_t>> sp;
  std::getline(g, line);
  while (std::getline(g, line)) { auto n = canonicalize(line); if (n.empty()) continue; sp.emplace_back(buf.size(), n.size()); buf += n; }
  std::vector<std::string_view> Q;
  for (auto [o, n] : sp) Q.emplace_back(buf.data() + o, n);
  const size_t n = Q.size();
  HashFib3 hash;
  hash.reserve(E.size());
  for (auto& e : E) hash.insert(e.first, e.second);
  GuardIndex gi;
  gi.build(E);
  const size_t C = std::stoul(argv[3]);
  const int reps = std::stoi(argv[4]);
  HotConfig hc; hc.capacity = C; hc.learningLimit = 0; hc.sampleEvery = 16; hc.lowerBound = true;
  size_t width = 2048; while (width < 4 * C) width <<= 1;
  const SketchPolicyH::Config sk{2, width, 4 * width, true};
  using Cache = HotPrefix3<SketchPolicyH, HashFib3, GuardIndex>;

  // untimed warm-up of both engines
  { uint64_t s = 0; for (size_t i = 0; i < std::min<size_t>(n, 30000); ++i) s += hash.lookup(Q[i]).face;
    Cache w(gi, hash, SketchPolicyH(sk), hc); for (size_t i = 0; i < std::min<size_t>(n, 30000); ++i) s += w.lookup(Q[i]).face;
    if (s == 42) std::cerr << ""; }

  std::ofstream out(argv[5], std::ios::app);
  out << std::fixed << std::setprecision(3);
  volatile uint64_t sink = 0;
  for (int rep = 1; rep <= reps; ++rep) {
    std::vector<float> h1, h2, cc;
    std::vector<uint8_t> hit(n);
    timed(n, [&](size_t i) { sink = sink + hash.lookup(Q[i]).face; }, h1);
    {
      Cache cache(gi, hash, SketchPolicyH(sk), hc);
      uint64_t prev = 0;
      timed(n, [&](size_t i) { sink = sink + cache.lookup(Q[i]).face; }, cc);
      // hit flags from a second, untimed identical run (deterministic: same stream, same seeds)
      Cache replay(gi, hash, SketchPolicyH(sk), hc);
      for (size_t i = 0; i < n; ++i) { replay.lookup(Q[i]); hit[i] = replay.hits() != prev; prev = replay.hits(); }
      if (replay.hits() != cache.hits()) { std::cerr << "non-deterministic cache run\n"; return 2; }
    }
    timed(n, [&](size_t i) { sink = sink + hash.lookup(Q[i]).face; }, h2);
    double sh = 0, nh = 0, scold = 0, nc = 0, th = 0, ch = 0, cm = 0, tc = 0;
    for (size_t i = 0; i < n; ++i) {
      const double hv = 0.5 * (double(h1[i]) + double(h2[i]));
      th += hv; tc += cc[i];
      if (hit[i]) { sh += hv; ++nh; ch += cc[i]; } else { scold += hv; ++nc; cm += cc[i]; }
    }
    out << argv[6] << ',' << C << ',' << rep << ',' << n << ',' << nh / n << ',' << sh / std::max(1.0, nh) << ','
        << scold / std::max(1.0, nc) << ',' << th / n << ',' << ch / std::max(1.0, nh) << ',' << cm / std::max(1.0, nc)
        << ',' << tc / n << '\n';
    std::cerr << argv[6] << " rep " << rep << " h=" << nh / n << " F_hot=" << sh / std::max(1.0, nh) << " F_cold="
              << scold / std::max(1.0, nc) << " T_hash=" << th / n << " c_hit=" << ch / std::max(1.0, nh)
              << " c_miss=" << cm / std::max(1.0, nc) << " T_cache=" << tc / n << "\n";
  }
  return 0;
}
