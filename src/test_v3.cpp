// test_v3.cpp -- correctness and equivalence tests for fib_v3.hpp
//  1. name parsing and prefix hashing invariants
//  2. every v3 engine returns the exact LPM (matched, face, depth) of the
//     reference trie on randomized FIBs with deep nesting
//  3. the v3 trie agrees with the original v2 trie
//  4. Hot-prefix v3 reproduces the original v2 HotPrefixEngine hit/miss trace,
//     cache size and eviction count query by query (B0, B1, B2; stationary
//     and decay configurations)
//  5. engines stay exact across FIB insertions and removals
#include "fib_v3.hpp"

#include <cassert>
#include <fstream>
#include <iostream>
#include <random>

using namespace fibv3;

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { ++failures; if (failures < 20) std::cerr << "FAIL: " << msg << "\n"; } } while (0)

struct RandomFib {
  std::vector<std::pair<std::string, int>> entries;
  std::vector<std::string> queries;
};

// Small alphabets and deep names force heavy nesting, guard hits and
// collisions of cached depths.
RandomFib makeRandom(uint64_t seed, size_t nPrefixes, size_t nQueries, int alphabet, int maxDepth,
                     bool zipfQueries) {
  std::mt19937_64 rng(seed);
  auto comp = [&](int a) { return std::string("c") + std::to_string(rng() % a); };
  std::set<std::string> seen;
  RandomFib r;
  size_t attempts = 0;
  while (r.entries.size() < nPrefixes && ++attempts < 50 * nPrefixes) {
    int d = 1 + static_cast<int>(rng() % maxDepth);
    std::string p;
    for (int i = 0; i < d; ++i) p += "/" + comp(i < 2 ? alphabet : alphabet * 2);
    if (seen.insert(p).second) r.entries.emplace_back(p, static_cast<int>(rng() % 64));
  }
  std::vector<double> cdf;
  double tot = 0;
  for (size_t i = 0; i < r.entries.size(); ++i) { tot += 1.0 / std::pow(i + 1.0, 1.0); cdf.push_back(tot); }
  std::uniform_real_distribution<double> U(0, tot);
  for (size_t i = 0; i < nQueries; ++i) {
    std::string q;
    if (zipfQueries && rng() % 4) {
      size_t k = std::lower_bound(cdf.begin(), cdf.end(), U(rng)) - cdf.begin();
      q = r.entries[std::min(k, r.entries.size() - 1)].first;
      int extra = static_cast<int>(rng() % 4);
      for (int j = 0; j < extra; ++j) q += "/" + comp(alphabet * 2);
    } else {
      int d = 1 + static_cast<int>(rng() % (maxDepth + 2));
      for (int j = 0; j < d; ++j) q += "/" + comp(j < 2 ? alphabet : alphabet * 2);
    }
    r.queries.push_back(q);
  }
  return r;
}

template <class Engine>
size_t checkEngine(Engine& e, const TrieFib3& ref, const std::vector<std::string>& qs, const char* name) {
  size_t bad = 0;
  for (const auto& q : qs) {
    const auto got = e.lookup(q), want = ref.lookup(q);
    if (!sameResult(got, want)) {
      if (++bad <= 3)
        std::cerr << name << " mismatch q=" << q << " got(" << got.matched << "," << got.depth << "," << got.face
                  << ") want(" << want.matched << "," << want.depth << "," << want.face << ")\n";
    }
  }
  CHECK(bad == 0, name << " LPM mismatches=" << bad);
  return bad;
}

struct Built {
  TrieFib3 trie;
  HashFib3 hash;
  HashFib2Stage two;
  fibv2::TrieFib trie2;
  fibv2::HashFibEngine hash2;
};

std::unique_ptr<Built> build(const std::vector<std::pair<std::string, int>>& entries) {
  auto b = std::make_unique<Built>();
  b->hash.reserve(entries.size());
  for (const auto& e : entries) {
    b->trie.insert(e.first, e.second);
    b->hash.insert(e.first, e.second);
    b->trie2.insert(e.first, e.second);
    b->hash2.insert(e.first, e.second);
  }
  b->two.build(entries);
  return b;
}

void testParsing() {
  NameView v;
  CHECK(parseName<true>("/a/bb/ccc", v) && v.m == 3, "parse m");
  CHECK(v.prefix(2) == "/a/bb" && v.comp(3) == "ccc" && v.comp(1) == "a", "parse views");
  CHECK(v.ph[2] == prefixKey("/a/bb"), "prefix hash of view equals hash of canonical prefix");
  CHECK(!parseName<true>("a/b", v) && !parseName<true>("/a//b", v) && !parseName<true>("/a/", v), "reject non-canonical");
  CHECK(canonicalize("//a///b/c/\r\n") == "/a/b/c", "canonicalize");
  CHECK(parseName<true>("", v) && v.m == 0, "empty name");
  // hash depends on component boundaries
  CHECK(prefixKey("/ab/c") != prefixKey("/a/bc"), "boundary-sensitive hash");
  // SWAR parser vs a simple reference on random names of all lengths mod 8
  std::mt19937_64 rng(7);
  size_t bad = 0;
  std::unordered_set<uint64_t> keys;
  std::unordered_set<std::string> names;
  for (int t = 0; t < 200000; ++t) {
    int m = 1 + static_cast<int>(rng() % 12);
    std::string q;
    std::vector<std::string> comps;
    for (int k = 0; k < m; ++k) {
      int len = 1 + static_cast<int>(rng() % 19);
      std::string c;
      for (int j = 0; j < len; ++j) c.push_back("abcdefgh0123456789-_."[rng() % 21]);
      comps.push_back(c);
      q += "/" + c;
    }
    NameView v;
    if (!parseName<true>(q, v) || v.m != m) { ++bad; continue; }
    for (int k = 1; k <= m; ++k) {
      if (v.comp(k) != comps[k - 1]) ++bad;
      if (v.ph[k] != prefixKey(std::string(v.prefix(k)))) ++bad;
    }
    NameView w;
    if (!parseName<false>(q, w) || w.m != m || w.end[m] != v.end[m]) ++bad;
    if (names.insert(q).second) keys.insert(v.ph[m]);
  }
  CHECK(bad == 0, "SWAR parser mismatches=" << bad);
  CHECK(keys.size() == names.size(), "64-bit prefix-hash collisions among random names: " << names.size() - keys.size());
}

template <class Policy2, class Policy3>
void traceEquivalence(const Built& b, const std::vector<std::string>& qs, Policy2 p2, Policy3 p3,
                      size_t cap, double alpha, double cool, const char* label) {
  fibv2::HotPrefixEngine<Policy2> v2(b.trie2, std::move(p2), cap, 4, alpha, cool);
  // policy-injection constructor has no hash fallback; results are exact either way
  HotConfig cfg; cfg.capacity = cap; cfg.alpha = alpha; cfg.cool = cool; cfg.lowerBound = true;
  HotPrefix3<Policy3, HashFib3> v3(b.trie, b.hash, std::move(p3), cfg);
  size_t diffs = 0, hits = 0;
  for (size_t i = 0; i < qs.size(); ++i) {
    const uint64_t h2 = v2.fastHits();
    const auto r2 = v2.lookup(qs[i]);
    const uint64_t h3 = v3.hits();
    const auto r3 = v3.lookup(qs[i]);
    const bool hit2 = v2.fastHits() != h2, hit3 = v3.hits() != h3;
    hits += hit3;
    const bool same = hit2 == hit3 && v2.cacheSize() == v3.cacheSize() && v2.evictions() == v3.evictions() &&
                      r2.matched == r3.matched && (!r2.matched || (r2.face == r3.face && r2.depth == r3.depth));
    if (!same && ++diffs <= 3)
      std::cerr << label << " trace diff at " << i << " q=" << qs[i] << " hit " << hit2 << "/" << hit3
                << " size " << v2.cacheSize() << "/" << v3.cacheSize() << " ev " << v2.evictions() << "/"
                << v3.evictions() << "\n";
  }
  CHECK(diffs == 0, label << " trace differences=" << diffs);
  std::cout << "  trace " << label << ": " << qs.size() << " queries, hits=" << hits << ", diffs=" << diffs
            << ", guardRejects=" << v3.guardRejects() << ", residentFallbacks=" << v3.residentFallbacks()
            << ", lbResolves=" << v3.lbResolves() << "\n";
}

void testRandom(uint64_t seed, int alphabet, int maxDepth, size_t cap) {
  auto r = makeRandom(seed, 3000, 60000, alphabet, maxDepth, true);
  auto b = build(r.entries);
  size_t nested = 0;
  for (const auto& e : r.entries) if (!b->trie.guardComponents(e.first).empty()) ++nested;
  std::cout << "random seed=" << seed << " alphabet=" << alphabet << " maxDepth=" << maxDepth
            << " prefixes=" << r.entries.size() << " withDescendants=" << nested << "\n";
  // v3 trie vs v2 trie
  size_t bad = 0;
  for (const auto& q : r.queries) {
    auto a = b->trie.lookup(q);
    auto c = b->trie2.lookup(q);
    if (a.matched != c.matched || (a.matched && (a.face != c.face || a.depth != c.depth))) ++bad;
  }
  CHECK(bad == 0, "v3 trie vs v2 trie mismatches=" << bad);
  checkEngine(b->hash, b->trie, r.queries, "hash3");
  checkEngine(b->two, b->trie, r.queries, "hash2stage");
  HotConfig cfg; cfg.capacity = cap;
  {
    HotPrefix3<fibv2::BoundedExactPolicy, HashFib3> e(b->trie, b->hash, fibv2::BoundedExactPolicy(4 * cap, 1.0), cfg);
    checkEngine(e, b->trie, r.queries, "hot_b1");
  }
  {
    HotPrefix3<fibv2::BoundedFrequencySketch, HashFib2Stage> e(
        b->trie, b->two, fibv2::BoundedFrequencySketch(fibv2::BoundedFrequencySketch::Config{2, 2048, true, 8192, true}), cfg);
    checkEngine(e, b->trie, r.queries, "hot_b2_2stage");
  }
  {
    HotConfig c2 = cfg; c2.alpha = 0.995; c2.cool = 0.25;
    HotPrefix3<fibv2::BoundedFrequencySketch, HashFib3> e(b->trie, b->hash, fibv2::BoundedFrequencySketch(4 * cap, 0.995), c2);
    checkEngine(e, b->trie, r.queries, "hot_b2_decay");
  }
  {
    HotPrefix3<AdmitAllPolicy, HashFib3> e(b->trie, b->hash, AdmitAllPolicy(0, 1.0), cfg);
    checkEngine(e, b->trie, r.queries, "hot_admit_all");
  }
  for (int lbOn = 0; lbOn < 2; ++lbOn) {
    // lower-bound fallback over both fallback types, stationary and decay
    HotConfig c1 = cfg; c1.lowerBound = lbOn; c1.learningLimit = 0; c1.sampleEvery = 3;
    HotPrefix3<SketchPolicyH, HashFib3> a(b->trie, b->hash, SketchPolicyH(SketchPolicyH::Config{2, 4096, 16384, true}), c1);
    checkEngine(a, b->trie, r.queries, lbOn ? "hpx_lb" : "hpx_nolb");
    HotPrefix3<SketchPolicyH, HashFib2Stage> t(b->trie, b->two, SketchPolicyH(SketchPolicyH::Config{}), c1);
    checkEngine(t, b->trie, r.queries, lbOn ? "hpx_lb_2stage" : "hpx_nolb_2stage");
    HotConfig c2 = c1; c2.alpha = 0.995; c2.cool = 0.25;
    HotPrefix3<SketchPolicyH, HashFib3> dcy(b->trie, b->hash, SketchPolicyH(SketchPolicyH::Config{2, 2048, 138, true}), c2);
    checkEngine(dcy, b->trie, r.queries, lbOn ? "hpx_lb_decay" : "hpx_nolb_decay");
    if (lbOn) CHECK(a.lbResolves() > 0 || nested == 0, "lower-bound path never exercised");
    // Stage B: fill threshold, unit-consistent scores, adaptive bypass (short epochs so it switches)
    HotConfig cb = c1; cb.sampleEvery = 4; cb.thetaFree = 2; cb.scaleBySample = true; cb.sharedAging = true;
    cb.adaptive = true; cb.epochLen = 1024; cb.exploreEvery = 2; cb.margin = -0.5;  // margin < 0 forces frequent bypassing
    HotPrefix3<SketchPolicyH, HashFib3> sb(b->trie, b->hash, SketchPolicyH(SketchPolicyH::Config{2, 1024, 512, true}), cb);
    checkEngine(sb, b->trie, r.queries, lbOn ? "hpb_lb" : "hpb_nolb");
    cb.margin = 0.03;
    HotPrefix3<SketchPolicyH, HashFib2Stage> sb2(b->trie, b->two, SketchPolicyH(SketchPolicyH::Config{2, 1024, 512, true}), cb);
    checkEngine(sb2, b->trie, r.queries, "hpb_2stage");
    if (lbOn) {
      CHECK(sb.bypassLookups() > 0 && sb.trials() > 0 && sb.switches() > 0, "bypass/exploration never exercised");
      std::cout << "  hpb bypassLookups=" << sb.bypassLookups() << " explorations=" << sb.trials() << " switches=" << sb.switches() << " hits=" << sb.hits()
                << " size=" << sb.cacheSize() << "\n";
    }
    if (lbOn) std::cout << "  hpx_lb lbResolves=" << a.lbResolves() << " guardRejects=" << a.guardRejects()
                        << " decay lbResolves=" << dcy.lbResolves() << "\n";
  }
  { Kim3<HashFib3> e(b->trie, b->hash, cap); checkEngine(e, b->trie, r.queries, "kim3"); }
  { Kim3<HashFib3> e(b->trie, b->hash, cap, 8, 2); checkEngine(e, b->trie, r.queries, "kim3_8bit"); }
  { Pbc3<HashFib3> e(b->trie, b->hash, cap); checkEngine(e, b->trie, r.queries, "pbc3"); }
  { FullNameCache3<HashFib3> e(b->trie, b->hash, cap); checkEngine(e, b->trie, r.queries, "fullname3"); }
  {
    GuardIndex gi; gi.build(r.entries);
    CHECK(gi.withDescendants() == nested, "guard index size " << gi.withDescendants() << " vs " << nested);
    size_t gbad = 0;
    for (const auto& e : r.entries) {
      auto a = gi.guardComponents(e.first), t = b->trie.guardComponents(e.first);
      std::sort(t.begin(), t.end());
      if (a != t) ++gbad;
    }
    CHECK(gbad == 0, "guard index differs from trie guards: " << gbad);
    HotPrefix3<fibv2::BoundedFrequencySketch, HashFib3, GuardIndex> e(
        gi, b->hash, fibv2::BoundedFrequencySketch(fibv2::BoundedFrequencySketch::Config{2, 2048, true, 8192, true}), cfg);
    checkEngine(e, b->trie, r.queries, "hot_b2_guardindex");
    HotPrefix3<SketchPolicyH, HashFib3, GuardIndex> eh(gi, b->hash, SketchPolicyH(SketchPolicyH::Config{}), cfg);
    checkEngine(eh, b->trie, r.queries, "hot_b2h_guardindex");
    HotConfig cs = cfg; cs.sampleEvery = 4; cs.learningLimit = 0;
    HotPrefix3<SketchPolicyH, HashFib3, GuardIndex> es(gi, b->hash, SketchPolicyH(SketchPolicyH::Config{}), cs);
    checkEngine(es, b->trie, r.queries, "hot_b2h_sampled");
    std::cout << "  hot_b2h hits=" << eh.hits() << " (B2 hits=" << e.hits() << ")\n";
    Kim3<HashFib3, GuardIndex> k(gi, b->hash, cap); checkEngine(k, b->trie, r.queries, "kim3_guardindex");
    Pbc3<HashFib3, GuardIndex> pb(gi, b->hash, cap); checkEngine(pb, b->trie, r.queries, "pbc3_guardindex");
  }

  traceEquivalence(*b, r.queries, fibv2::BoundedExactPolicy(4 * cap, 1.0), fibv2::BoundedExactPolicy(4 * cap, 1.0),
                   cap, 1.0, 0.0, "B1 stationary");
  traceEquivalence(*b, r.queries, fibv2::UnboundedCandidatePolicy(4 * cap, 1.0),
                   fibv2::UnboundedCandidatePolicy(4 * cap, 1.0), cap, 1.0, 0.0, "B0 stationary");
  fibv2::BoundedFrequencySketch::Config sc{2, 2048, true, 8192, true};
  traceEquivalence(*b, r.queries, fibv2::BoundedFrequencySketch(sc), fibv2::BoundedFrequencySketch(sc), cap, 1.0, 0.0,
                   "B2 stationary");
  traceEquivalence(*b, r.queries, fibv2::BoundedExactPolicy(4 * cap, 0.995), fibv2::BoundedExactPolicy(4 * cap, 0.995),
                   cap, 0.995, 0.25, "B1 decay");
  traceEquivalence(*b, r.queries, fibv2::BoundedFrequencySketch(4 * cap, 0.995),
                   fibv2::BoundedFrequencySketch(4 * cap, 0.995), cap, 0.995, 0.25, "B2 decay");
}

void testUpdates(uint64_t seed) {
  auto r = makeRandom(seed, 2000, 40000, 5, 6, true);
  TrieFib3 trie;
  HashFib3 hash;
  for (const auto& e : r.entries) { trie.insert(e.first, e.second); hash.insert(e.first, e.second); }
  HotConfig cfg; cfg.capacity = 128; cfg.learningLimit = 0; cfg.lowerBound = true;
  HotPrefix3<fibv2::BoundedExactPolicy, HashFib3> hot(trie, hash, fibv2::BoundedExactPolicy(512, 1.0), cfg);
  HotConfig cbu = cfg; cbu.sampleEvery = 4; cbu.thetaFree = 2; cbu.scaleBySample = cbu.sharedAging = true;
  cbu.adaptive = true; cbu.epochLen = 1024; cbu.exploreEvery = 2; cbu.margin = -0.5;
  HotPrefix3<SketchPolicyH, HashFib3> hpb(trie, hash, SketchPolicyH(SketchPolicyH::Config{2, 1024, 512, true}), cbu);
  Kim3<HashFib3> kim(trie, hash, 128);
  Pbc3<HashFib3> pbc(trie, hash, 128);
  FullNameCache3<HashFib3> full(trie, hash, 128);
  std::mt19937_64 rng(seed + 99);
  size_t bad = 0;
  for (size_t i = 0; i < r.queries.size(); ++i) {
    if (i % 997 == 0) {
      // remove or re-add a random prefix, or add a new nested one
      const auto& e = r.entries[rng() % r.entries.size()];
      if (rng() % 2) { trie.remove(e.first); hash.remove(e.first); }
      else {
        std::string p = e.first + "/u" + std::to_string(rng() % 3);
        trie.insert(p, 63); hash.insert(p, 63);
        trie.insert(e.first, e.second); hash.insert(e.first, e.second);
      }
    }
    const auto want = trie.lookup(r.queries[i]);
    bad += !sameResult(hot.lookup(r.queries[i]), want);
    bad += !sameResult(hpb.lookup(r.queries[i]), want);
    bad += !sameResult(kim.lookup(r.queries[i]), want);
    bad += !sameResult(pbc.lookup(r.queries[i]), want);
    bad += !sameResult(full.lookup(r.queries[i]), want);
    bad += !sameResult(hash.lookup(r.queries[i]), want);
  }
  CHECK(bad == 0, "update test mismatches=" << bad);
  std::cout << "updates seed=" << seed << ": mismatches=" << bad << ", hot hits=" << hot.hits() << "\n";
}

// Trace equivalence on a dataset file (FIB csv + query csv), e.g. the paper's.
void testDataset(const std::string& fibCsv, const std::string& qCsv, size_t limit) {
  std::ifstream f(fibCsv), g(qCsv);
  if (!f || !g) { std::cout << "dataset not found, skipped\n"; return; }
  std::vector<std::pair<std::string, int>> entries;
  std::string line;
  std::getline(f, line);
  while (std::getline(f, line)) {
    auto c = line.rfind(',');
    entries.emplace_back(canonicalize(line.substr(0, c)), std::stoi(line.substr(c + 1)));
  }
  std::vector<std::string> qs;
  std::getline(g, line);
  while (std::getline(g, line) && qs.size() < limit) qs.push_back(canonicalize(line));
  auto b = build(entries);
  checkEngine(b->hash, b->trie, qs, "dataset hash3");
  traceEquivalence(*b, qs, fibv2::BoundedExactPolicy(2048, 1.0), fibv2::BoundedExactPolicy(2048, 1.0), 512, 1.0, 0.0,
                   "dataset B1");
  fibv2::BoundedFrequencySketch::Config sc{2, 2048, true, 8192, true};
  traceEquivalence(*b, qs, fibv2::BoundedFrequencySketch(sc), fibv2::BoundedFrequencySketch(sc), 512, 1.0, 0.0,
                   "dataset B2");
}

// Stage C: incremental maintenance under heavy churn. After every update the
// cache must satisfy I1-I3, and every lookup must equal the reference trie.
template <class Policy>
void testIncremental(uint64_t seed, Policy pol, const char* label, bool sampled) {
  auto r = makeRandom(seed, 2500, 80000, 4, 7, true);
  TrieFib3 trie;
  HashFib3 hash;
  for (const auto& e : r.entries) { trie.insert(e.first, e.second); hash.insert(e.first, e.second); }
  HotConfig cfg; cfg.capacity = 96; cfg.learningLimit = 0; cfg.lowerBound = true; cfg.incremental = true;
  if (sampled) cfg.sampleEvery = 4;
  HotPrefix3<Policy, HashFib3> e(trie, hash, std::move(pol), cfg);
  std::mt19937_64 rng(seed * 31 + 7);
  std::vector<std::string> removed;
  size_t bad = 0, viol = 0, ops = 0;
  for (size_t i = 0; i < r.queries.size(); ++i) {
    if (i % 50 == 0 && i) {
      std::string x;
      const int op = static_cast<int>(rng() % 4);
      if (op == 0) {                         // remove a registered prefix
        x = r.entries[rng() % r.entries.size()].first;
        if (trie.remove(x)) { hash.remove(x); removed.push_back(x); } else x.clear();
      } else if (op == 1 && !removed.empty()) {  // re-insert a removed prefix
        x = removed.back(); removed.pop_back();
        trie.insert(x, 77); hash.insert(x, 77);
      } else if (op == 2) {                  // new nested prefix under a registered one
        x = r.entries[rng() % r.entries.size()].first + "/n" + std::to_string(rng() % 6);
        if (rng() % 2) x += "/c" + std::to_string(rng() % 4);
        trie.insert(x, static_cast<int>(rng() % 64)); hash.insert(x, 5);
        hash.remove(x); hash.insert(x, trie.lookup(x).face);  // keep faces identical
      } else {                               // face change of a registered prefix
        x = r.entries[rng() % r.entries.size()].first;
        if (trie.lookup(x).matched && trie.lookup(x).depth == static_cast<int>(std::count(x.begin(), x.end(), '/'))) {
          const int f = static_cast<int>(rng() % 64);
          trie.insert(x, f); hash.insert(x, f);
        } else x.clear();
      }
      if (!x.empty()) { e.onFibUpdate(x); ++ops; viol += e.invariantViolations(); }
    }
    bad += !sameResult(e.lookup(r.queries[i]), trie.lookup(r.queries[i]));
  }
  CHECK(bad == 0, label << " incremental: LPM mismatches=" << bad);
  CHECK(viol == 0, label << " incremental: invariant violations=" << viol);
  CHECK(e.hits() > r.queries.size() / 10, label << " incremental: cache was effectively flushed");
  std::cout << "incremental " << label << " seed=" << seed << ": updates=" << ops << " hits=" << e.hits()
            << " invalidations=" << e.invalidations() << " guardRefreshes=" << e.guardRefreshes()
            << " mismatches=" << bad << " violations=" << viol << "\n";
}

int main(int argc, char** argv) {
  testParsing();
  testRandom(1, 3, 6, 64);
  testRandom(2, 4, 8, 128);
  testRandom(3, 8, 10, 256);
  testRandom(4, 2, 5, 32);
  testUpdates(5);
  testUpdates(6);
  for (uint64_t sd : {11, 12, 13}) {
    testIncremental(sd, fibv2::BoundedExactPolicy(384, 1.0), "B1", false);
    testIncremental(sd, SketchPolicyH(SketchPolicyH::Config{2, 1024, 4096, true}), "B2h-sampled", true);
  }
  if (argc >= 3) testDataset(argv[1], argv[2], argc >= 4 ? std::stoul(argv[3]) : 100000);
  std::cout << (failures ? "FAILED " : "ALL TESTS PASSED ") << failures << "\n";
  return failures ? 1 : 0;
}
