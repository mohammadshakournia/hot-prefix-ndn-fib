// ctl_debug.cpp -- prints the adaptive controller's per-epoch cost estimates
#include "../../src/fib_v3.hpp"
#include <fstream>
#include <iostream>
using namespace fibv3;
int main(int argc, char** argv) {
  std::ifstream f(argv[1]), g(argv[2]); std::string line; std::vector<std::pair<std::string,int>> E;
  std::getline(f, line); while (std::getline(f, line)) { auto c = line.rfind(','); E.emplace_back(canonicalize(line.substr(0, c)), std::stoi(line.substr(c + 1))); }
  std::string buf; std::vector<std::pair<size_t,size_t>> sp; std::getline(g, line);
  while (std::getline(g, line)) { auto n = canonicalize(line); sp.emplace_back(buf.size(), n.size()); buf += n; }
  std::vector<std::string_view> Q; for (auto [o, n] : sp) Q.emplace_back(buf.data() + o, n);
  HashFib3 hash; hash.reserve(E.size()); for (auto& e : E) hash.insert(e.first, e.second);
  GuardIndex gi; gi.build(E);
  HotConfig hc; hc.capacity = 1024; hc.learningLimit = 0; hc.sampleEvery = 16; hc.adaptive = true;
  HotPrefix3<SketchPolicyH, HashFib3, GuardIndex> e(gi, hash, SketchPolicyH(SketchPolicyH::Config{2, 4096, 16384, true}), hc);
  uint64_t chk = 0; double lo = -1;
  auto t0 = std::chrono::steady_clock::now();
  for (size_t i = 0; i < Q.size(); ++i) {
    chk += e.lookup(Q[i]).face;
    if ((i + 1) % 16384 == 0) std::cout << "epoch " << (i + 1) / 16384 << " explore=" << e.lastExplore() << " bracket=" << e.lastBracket() << " bypassMode=" << e.inBypass() << " switches=" << e.switches() << " hits=" << e.hits() << "\n";
  }
  auto t1 = std::chrono::steady_clock::now();
  for (size_t i = 0; i < Q.size(); ++i) chk += hash.lookup(Q[i]).face;
  auto t2 = std::chrono::steady_clock::now();
  std::cout << "hpb " << std::chrono::duration<double, std::nano>(t1 - t0).count() / Q.size() << " ns, hash " << std::chrono::duration<double, std::nano>(t2 - t1).count() / Q.size() << " ns (" << (chk & 1) << ")\n";
  (void)lo;
}
