// bench_v3.cpp -- benchmark harness for fib_v3.hpp
//
// Modes
//   timeline : single pass from an empty software cache; lookups timed in
//              windows; per-window and per-run CSV output
//   tail     : single pass; every lookup timed individually with the TSC
//              (lfence-serialized, calibrated overhead subtracted); per-run
//              percentiles
//   mem      : (bench_v3_mem build, -DFIBV3_MEMCOUNT) allocator-counted live
//              bytes of FIB structures and of each engine's state after the pass
// Options
//   --fib F --queries Q --engines a,b,c --runs R --seed S --cache C --L L
//   --window W --segments first,last --update-every U --tag k=v,... --out PREFIX
//
// Every run builds fresh engines, runs them in a seeded random order, and
// validates every result of an untimed replay against the reference trie.
#include "fib_v3.hpp"

#include <x86intrin.h>

#include <charconv>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>

#ifdef FIBV3_MEMCOUNT
#include <malloc.h>
#ifdef _WIN32
#define FIBV3_USABLE_SIZE(p) _msize(p)
#else
#define FIBV3_USABLE_SIZE(p) malloc_usable_size(p)
#endif
static int64_t g_live = 0;
static bool g_count = true;
void* operator new(size_t n) {
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  if (g_count) g_live += static_cast<int64_t>(FIBV3_USABLE_SIZE(p));
  return p;
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept {
  if (!p) return;
  if (g_count) g_live -= static_cast<int64_t>(FIBV3_USABLE_SIZE(p));
  std::free(p);
}
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, size_t) noexcept { operator delete(p); }
void operator delete[](void* p, size_t) noexcept { operator delete(p); }
static int64_t liveBytes() { return g_live; }
#else
static int64_t liveBytes() { return 0; }
#endif

using namespace fibv3;

namespace {

struct Config {
  std::string fib, queries, out = "out", tag, mode = "timeline";
  std::vector<std::string> engines;
  size_t runs = 5, cache = 512, window = 1000, segFirst = 10000, segLast = 30000, updateEvery = 0;
  uint64_t seed = 1, L = 65536;
  int twoStageM = 0;
  bool noTrie = false;
  bool warmup = true;
  bool hotUpdates = false;
};

std::vector<std::string> splitList(const std::string& s) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string x;
  while (std::getline(ss, x, ',')) if (!x.empty()) out.push_back(x);
  return out;
}

struct Data {
  std::vector<std::pair<std::string, int>> fibEntries;
  TrieFib3 trie;
  GuardIndex guards;
  HashFib3 hash;
  HashFib2Stage two;
  std::unique_ptr<fibv2::TrieFib> trie2;
  std::unique_ptr<fibv2::HashFibEngine> hash2;
  std::string qbuf;
  std::vector<std::string_view> q;
  std::vector<std::string> qstr;  // only for v2 engines
  int64_t bytesTrie = 0, bytesHash = 0, bytesTwo = 0, bytesGuard = 0;
};

void load(const Config& c, Data& d, bool needV2) {
  std::ifstream f(c.fib);
  if (!f) throw std::runtime_error("cannot open " + c.fib);
  std::string line;
  std::getline(f, line);
  std::unordered_set<std::string> seen;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    const auto comma = line.rfind(',');
    std::string p = canonicalize(line.substr(0, comma));
    if (p.empty() || !seen.insert(p).second) continue;
    d.fibEntries.emplace_back(std::move(p), std::stoi(line.substr(comma + 1)));
  }
  int64_t b0 = liveBytes();
  if (!c.noTrie) for (const auto& e : d.fibEntries) d.trie.insert(e.first, e.second);
  int64_t b1 = liveBytes();
  d.hash.reserve(d.fibEntries.size());
  for (const auto& e : d.fibEntries) d.hash.insert(e.first, e.second);
  int64_t b2 = liveBytes();
  d.two.build(d.fibEntries, c.twoStageM);
  int64_t b3 = liveBytes();
  d.guards.build(d.fibEntries);
  int64_t b4 = liveBytes();
  d.bytesTrie = b1 - b0; d.bytesHash = b2 - b1; d.bytesTwo = b3 - b2; d.bytesGuard = b4 - b3;
  if (needV2) {
    d.trie2 = std::make_unique<fibv2::TrieFib>();
    d.hash2 = std::make_unique<fibv2::HashFibEngine>();
    for (const auto& e : d.fibEntries) { d.trie2->insert(e.first, e.second); d.hash2->insert(e.first, e.second); }
  }
  std::ifstream g(c.queries);
  if (!g) throw std::runtime_error("cannot open " + c.queries);
  std::getline(g, line);
  std::vector<std::pair<size_t, size_t>> spans;
  while (std::getline(g, line)) {
    std::string n = canonicalize(line);
    if (n.empty()) continue;
    spans.emplace_back(d.qbuf.size(), n.size());
    d.qbuf += n;
  }
  for (auto [o, n] : spans) d.q.emplace_back(d.qbuf.data() + o, n);
  if (needV2) for (auto s : d.q) d.qstr.emplace_back(s);
}

// ----- engine adapters ------------------------------------------------------
template <class E>
struct V2Hot {
  E e;
  const std::vector<std::string>* qs;
  LpmResult lookupIndex(size_t i) {
    auto r = e.lookup((*qs)[i]);
    return {r.face, static_cast<int16_t>(r.depth), r.matched};
  }
  uint64_t hits() const { return e.fastHits(); }
  uint64_t slow() const { return e.slowLookups(); }
  size_t cacheSize() const { return e.cacheSize(); }
  uint64_t evictions() const { return e.evictions(); }
};
struct V2Hash {
  const fibv2::HashFibEngine* h;
  const std::vector<std::string>* qs;
  LpmResult lookupIndex(size_t i) {
    auto r = h->lookup((*qs)[i]);
    return {r.face, static_cast<int16_t>(r.depth), r.matched};
  }
};
struct V2Trie {
  const fibv2::TrieFib* t;
  const std::vector<std::string>* qs;
  LpmResult lookupIndex(size_t i) {
    auto r = t->lookup((*qs)[i]);
    return {r.face, static_cast<int16_t>(r.depth), r.matched};
  }
};
struct TrieView { const TrieFib3* t; LpmResult lookup(std::string_view n) const { return t->lookup(n); } };
struct HashView { const HashFib3* h; LpmResult lookup(std::string_view n) const { return h->lookup(n); } };
struct TwoView { const HashFib2Stage* h; LpmResult lookup(std::string_view n) const { return h->lookup(n); } };

template <class E>
inline LpmResult doLookup(E& e, size_t i, std::string_view n) {
  if constexpr (requires { e.lookupIndex(i); }) return e.lookupIndex(i);
  else return e.lookup(n);
}

struct Counters {
  uint64_t hits = 0, slow = 0, guardRejects = 0, residentFallbacks = 0, admissions = 0, evictions = 0,
           filterPositives = 0, falseMisses = 0, victimRejects = 0;
  int64_t entries = -1;
  bool cached = false;
  uint64_t fibProbes = 0, bypassLookups = 0, guardRefreshes = 0, invalidations = 0;
};
template <class E>
Counters counters(const E& e) {
  Counters c;
  if constexpr (requires { e.hits(); }) { c.hits = e.hits(); c.slow = e.slow(); c.cached = true; }
  if constexpr (requires { e.guardRejects(); }) c.guardRejects = e.guardRejects();
  if constexpr (requires { e.residentFallbacks(); }) c.residentFallbacks = e.residentFallbacks();
  if constexpr (requires { e.admissions(); }) c.admissions = e.admissions();
  if constexpr (requires { e.evictions(); }) c.evictions = e.evictions();
  if constexpr (requires { e.victimRejects(); }) c.victimRejects = e.victimRejects();
  if constexpr (requires { e.filterPositives(); }) c.filterPositives = e.filterPositives();
  if constexpr (requires { e.bitmapPositives(); }) c.filterPositives = e.bitmapPositives();
  if constexpr (requires { e.falseMisses(); }) c.falseMisses = e.falseMisses();
  if constexpr (requires { e.lbResolves(); }) c.falseMisses = e.lbResolves();  // column reused: lower-bound resolves
  if constexpr (requires { e.bypassLookups(); }) c.bypassLookups = e.bypassLookups();
  if constexpr (requires { e.guardRefreshes(); }) { c.guardRefreshes = e.guardRefreshes(); c.invalidations = e.invalidations(); }
  if constexpr (requires { e.cacheSize(); }) c.entries = static_cast<int64_t>(e.cacheSize());
  return c;
}

// ----- FIB updates (applied between windows, outside the timer) -------------
struct Updater {
  // Alternately removes a prefix and re-inserts it. 'uniform': a random FIB
  // entry; 'hot': the prefix the current request resolves to (updates follow
  // popularity). step() returns the updated prefix (the FIB is already changed).
  Data* d = nullptr;
  bool hot = false;
  std::vector<size_t> order;
  size_t k = 0;
  bool removed = false;
  std::string victim;
  int victimFace = 0;
  void reset(uint64_t seed) {
    order.resize(d->fibEntries.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::mt19937_64 rng(seed ^ 0xABCDEF);
    std::shuffle(order.begin(), order.end(), rng);
    k = 0; removed = false;
  }
  std::string_view step(size_t pos) {
    if (!removed) {
      victim.clear();
      if (hot) {
        NameView q;
        const auto& name = d->q[std::min(pos, d->q.size() - 1)];
        if (parseName<true>(name, q)) {
          const auto r = d->hash.resolve(q);
          if (r.matched) { victim.assign(q.prefix(r.depth)); victimFace = r.face; }
        }
      }
      if (victim.empty()) {
        const auto& e = d->fibEntries[order[k++ % order.size()]];
        victim = e.first; victimFace = e.second;
      }
      d->trie.remove(victim);
      d->hash.remove(victim);
      removed = true;
    } else {
      d->trie.insert(victim, victimFace);
      d->hash.insert(victim, victimFace);
      removed = false;
    }
    return victim;
  }
  std::string_view finish() { return removed ? step(0) : std::string_view{}; }
};

template <class E>
double notifyUpdate(E& e, std::string_view x) {
  if constexpr (requires { e.onFibUpdate(x); }) {
    const auto b = std::chrono::steady_clock::now();
    e.onFibUpdate(x);
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - b).count();
  } else {
    return 0.0;
  }
}

struct RunOut {
  std::vector<double> windowNs;
  std::vector<Counters> windowCounters;
  Counters total;
  size_t errors = 0;
  double tscNsPerTick = 0;
  std::vector<uint32_t> ticks;
  std::vector<uint8_t> hitFlag;
  int64_t stateBytes = 0;
};

double g_tscNs = 0;   // ns per TSC tick
double g_tscOverhead = 0;  // ticks
volatile uint64_t g_sink = 0;

void calibrateTsc() {
  auto c0 = std::chrono::steady_clock::now();
  uint64_t t0 = __rdtsc();
  while (std::chrono::steady_clock::now() - c0 < std::chrono::milliseconds(200)) {}
  uint64_t t1 = __rdtsc();
  auto c1 = std::chrono::steady_clock::now();
  g_tscNs = std::chrono::duration<double, std::nano>(c1 - c0).count() / static_cast<double>(t1 - t0);
  std::vector<uint64_t> o(20000);
  for (auto& x : o) {
    _mm_lfence(); uint64_t a = __rdtsc(); _mm_lfence();
    _mm_lfence(); uint64_t b = __rdtsc(); _mm_lfence();
    x = b - a;
  }
  std::nth_element(o.begin(), o.begin() + o.size() / 2, o.end());
  g_tscOverhead = static_cast<double>(o[o.size() / 2]);
}

template <class E>
RunOut runEngine(E& e, Data& d, const Config& c, bool tail, Updater* up) {
  RunOut out;
  d.hash.resetProbes();
  d.two.resetProbes();
  const size_t n = d.q.size();
  uint64_t chk = 0;
  if (tail) {
    out.ticks.resize(n);
    out.hitFlag.resize(n);
    uint64_t prevHits = 0;
    for (size_t i = 0; i < n; ++i) {
      _mm_lfence(); const uint64_t a = __rdtsc(); _mm_lfence();
      const auto r = doLookup(e, i, d.q[i]);
      _mm_lfence(); const uint64_t b = __rdtsc(); _mm_lfence();
      chk += static_cast<uint64_t>(r.face + 2) * (r.depth + 1);
      out.ticks[i] = static_cast<uint32_t>(std::min<uint64_t>(b - a, 0xFFFFFFFFu));
      if constexpr (requires { e.hits(); }) {
        const uint64_t h = e.hits();
        out.hitFlag[i] = h != prevHits;
        prevHits = h;
      }
    }
  } else {
    for (size_t s = 0; s < n; s += c.window) {
      double extraNs = 0;
      if (up && c.updateEvery && s && s % c.updateEvery == 0) extraNs = notifyUpdate(e, up->step(s));
      const size_t t = std::min(n, s + c.window);
      const auto b = std::chrono::steady_clock::now();
      for (size_t i = s; i < t; ++i) {
        const auto r = doLookup(e, i, d.q[i]);
        chk = chk * 1315423911ULL + static_cast<uint64_t>(r.face + 2) * (r.depth + 1);
      }
      const auto f = std::chrono::steady_clock::now();
      out.windowNs.push_back(std::chrono::duration<double, std::nano>(f - b).count() + extraNs);
      out.windowCounters.push_back(counters(e));
    }
  }
  g_sink += chk;
  out.total = counters(e);
  out.total.fibProbes = d.hash.probes() + d.two.probes();
  return out;
}

template <class E>
size_t validate(E& e, Data& d, const Config& c, Updater* up) {
  size_t bad = 0;
  for (size_t i = 0; i < d.q.size(); ++i) {
    if (up && c.updateEvery && i && i % c.updateEvery == 0) notifyUpdate(e, up->step(i));
    const auto want = c.noTrie ? d.hash.lookup(d.q[i]) : d.trie.lookup(d.q[i]);
    if (!sameResult(doLookup(e, i, d.q[i]), want)) ++bad;
  }
  return bad;
}

using Factory = std::function<void(const std::function<void(void*)>&)>;

// Calls f(engine) for a freshly constructed engine of the given name.
template <class G, class F>
bool withEngineG(const G& gs, const std::string& fullName, Data& d, const Config& c, F&& f) {
  // optional per-engine overrides: name:L=<n>  (learning cutoff; 0 = none)
  // "<engine>#<tag>" runs <engine> under a distinct name (A/A controls)
  std::string name = fullName.substr(0, fullName.find('#'));
  // paper aliases for the final design (Stages A-C): continuous tracking sampled 1/16, no cutoff
  if (name == "hp") name = "hpx:L=0/S=16";
  else if (name == "hp_inc") name = "hpx:L=0/S=16/INC=1";
  else if (name == "hp_2s") name = "hpx_2s:L=0/S=16";
  uint64_t L = c.L;
  uint32_t sample = 1;
  int lowerBoundOpt = -1;  // -1: engine default (on for hpx*/hpb*, off for the earlier engines)
  const bool isB = fullName.rfind("hpb", 0) == 0;
  double thetaFree = 0.0;   // Stage B result: the fill threshold and scaled scores did not pay off;
  bool units = false, adaptive = isB;  // they stay available as ablations (TF=, U=1)
  double margin = 0.03;
  uint32_t bypassSample = 0;
  bool forceBypass = false;
  bool incremental = false;
  size_t widthMul = 4;
  if (auto colon = name.find(':'); colon != std::string::npos) {
    const std::string opt = name.substr(colon + 1);
    name = name.substr(0, colon);
    // options separated by '/', e.g. hot_b2h:L=0/S=8
    std::stringstream os(opt);
    std::string o;
    while (std::getline(os, o, '/')) {
      if (o.rfind("L=", 0) == 0) L = std::stoull(o.substr(2));
      else if (o.rfind("S=", 0) == 0) sample = static_cast<uint32_t>(std::stoul(o.substr(2)));
      else if (o.rfind("LB=", 0) == 0) lowerBoundOpt = o.substr(3) != "0";
      else if (o.rfind("W=", 0) == 0) widthMul = std::stoul(o.substr(2));
      else if (o.rfind("TF=", 0) == 0) thetaFree = std::stod(o.substr(3));
      else if (o.rfind("U=", 0) == 0) units = o.substr(2) != "0";
      else if (o.rfind("AB=", 0) == 0) adaptive = o.substr(3) != "0";
      else if (o.rfind("M=", 0) == 0) margin = std::stod(o.substr(2));
      else if (o.rfind("BS=", 0) == 0) bypassSample = static_cast<uint32_t>(std::stoul(o.substr(3)));
      else if (o == "FB=1") forceBypass = true;
      else if (o == "INC=1") incremental = true;
      else throw std::runtime_error("unknown engine option " + o);
    }
  }
  if (isB && fullName.find("L=") == std::string::npos) L = 0;        // hpb defaults: continuous,
  if (isB && fullName.find("S=") == std::string::npos) sample = 16;  // tracking sampled 1/16
  const bool lowerBound = lowerBoundOpt >= 0 ? lowerBoundOpt == 1 : (name.rfind("hpx", 0) == 0 || isB);
  HotConfig hc; hc.capacity = c.cache; hc.learningLimit = L; hc.sampleEvery = sample; hc.lowerBound = lowerBound;
  hc.thetaFree = thetaFree; hc.scaleBySample = units; hc.sharedAging = units; hc.adaptive = adaptive; hc.margin = margin; hc.bypassSampleEvery = bypassSample; hc.forceBypass = forceBypass; hc.incremental = incremental;
  // hpx: sketch width scales with C (min 2048), aging window = 4 x width (v2 ratio 8192/2048)
  size_t hpxWidth = 2048;
  while (hpxWidth < widthMul * c.cache) hpxWidth <<= 1;
  const SketchPolicyH::Config hpxCfg{2, hpxWidth, 4 * hpxWidth, true};
  HotConfig hd = hc; hd.alpha = 0.995; hd.cool = 0.25;
  const fibv2::BoundedFrequencySketch::Config sk{2, 2048, true, 8192, true};
  if (name == "trie") { TrieView e{&d.trie}; f(e); }
  else if (name == "hash") { HashView e{&d.hash}; f(e); }
  else if (name == "hash2s") { TwoView e{&d.two}; f(e); }
  else if (name == "kim") { Kim3<HashFib3, G> e(gs, d.hash, c.cache); f(e); }
  else if (name == "kim8") { Kim3<HashFib3, G> e(gs, d.hash, c.cache, 8, 2); f(e); }
  else if (name == "pbc") { Pbc3<HashFib3, G> e(gs, d.hash, c.cache); f(e); }
  else if (name == "full") { FullNameCache3<HashFib3, G> e(gs, d.hash, c.cache); f(e); }
  else if (name == "hot_b0") { HotPrefix3<fibv2::UnboundedCandidatePolicy, HashFib3, G> e(gs, d.hash, fibv2::UnboundedCandidatePolicy(4 * c.cache, 1.0), hc); f(e); }
  else if (name == "hot_b1") { HotPrefix3<fibv2::BoundedExactPolicy, HashFib3, G> e(gs, d.hash, fibv2::BoundedExactPolicy(4 * c.cache, 1.0), hc); f(e); }
  else if (name == "hot_b2") { HotPrefix3<fibv2::BoundedFrequencySketch, HashFib3, G> e(gs, d.hash, fibv2::BoundedFrequencySketch(sk), hc); f(e); }
  else if (name == "hot_b2h") { HotPrefix3<SketchPolicyH, HashFib3, G> e(gs, d.hash, SketchPolicyH(SketchPolicyH::Config{2, 2048, 8192, true}), hc); f(e); }
  else if (name == "hot_b2h_big") { HotPrefix3<SketchPolicyH, HashFib3, G> e(gs, d.hash, SketchPolicyH(SketchPolicyH::Config{2, std::max<size_t>(2048, 8 * c.cache), std::max<uint64_t>(8192, 32 * c.cache), true}), hc); f(e); }
  else if (name == "hpb") { HotPrefix3<SketchPolicyH, HashFib3, G> e(gs, d.hash, SketchPolicyH(hpxCfg), hc); f(e); }
  else if (name == "hpb_2s") { HotPrefix3<SketchPolicyH, HashFib2Stage, G> e(gs, d.two, SketchPolicyH(hpxCfg), hc); f(e); }
  else if (name == "hpx") { HotPrefix3<SketchPolicyH, HashFib3, G> e(gs, d.hash, SketchPolicyH(hpxCfg), hc); f(e); }
  else if (name == "hpx_2s") { HotPrefix3<SketchPolicyH, HashFib2Stage, G> e(gs, d.two, SketchPolicyH(hpxCfg), hc); f(e); }
  else if (name == "hot_b2h_2s") { HotPrefix3<SketchPolicyH, HashFib2Stage, G> e(gs, d.two, SketchPolicyH(SketchPolicyH::Config{2, 2048, 8192, true}), hc); f(e); }
  else if (name == "hot_b2h_decay") { HotPrefix3<SketchPolicyH, HashFib3, G> e(gs, d.hash, SketchPolicyH(SketchPolicyH::Config{2, 2048, 138, true}), hd); f(e); }
  else if (name == "hot_all") { HotPrefix3<AdmitAllPolicy, HashFib3, G> e(gs, d.hash, AdmitAllPolicy(0, 1.0), hc); f(e); }
  else if (name == "hot_b2_2s") { HotPrefix3<fibv2::BoundedFrequencySketch, HashFib2Stage, G> e(gs, d.two, fibv2::BoundedFrequencySketch(sk), hc); f(e); }
  else if (name == "kim_2s") { Kim3<HashFib2Stage, G> e(gs, d.two, c.cache); f(e); }
  else if (name == "pbc_2s") { Pbc3<HashFib2Stage, G> e(gs, d.two, c.cache); f(e); }
  else if (name == "hot_b1_decay") { HotPrefix3<fibv2::BoundedExactPolicy, HashFib3, G> e(gs, d.hash, fibv2::BoundedExactPolicy(4 * c.cache, 0.995), hd); f(e); }
  else if (name == "hot_b2_decay") { HotPrefix3<fibv2::BoundedFrequencySketch, HashFib3, G> e(gs, d.hash, fibv2::BoundedFrequencySketch(4 * c.cache, 0.995), hd); f(e); }
  else if (name == "v2_trie") { V2Trie e{d.trie2.get(), &d.qstr}; f(e); }
  else if (name == "v2_hash") { V2Hash e{d.hash2.get(), &d.qstr}; f(e); }
  else if (name == "v2_hot_b1") { V2Hot<fibv2::HotPrefixEngineB1> e{fibv2::HotPrefixEngineB1(*d.trie2, *d.hash2, c.cache, 4, 1.0, 0.0), &d.qstr}; f(e); }
  else if (name == "v2_hot_b2") { V2Hot<fibv2::HotPrefixEngineB2> e{fibv2::HotPrefixEngineB2(*d.trie2, *d.hash2, fibv2::BoundedFrequencySketch(sk), c.cache, 4, 1.0, 0.0), &d.qstr}; f(e); }
  else return false;
  return true;
}

template <class F>
bool withEngine(const std::string& name, Data& d, const Config& c, F&& f) {
  if (name == "trie" && c.noTrie) throw std::runtime_error("trie engine requires the trie");
  if (c.updateEvery) return withEngineG(d.trie, name, d, c, f);  // dynamic guards
  return withEngineG(d.guards, name, d, c, f);
}

// ----- interleaved measurement -----------------------------------------------
// All engines advance through the stream in lockstep: the stream is cut into
// slices of --window lookups, and in every slice each engine processes that
// slice once, in a fresh random engine order per slice. Slow drift of the
// machine (frequency, co-tenants) therefore affects all engines alike.
struct Holder {
  std::shared_ptr<void> keep;
  std::function<double(size_t, size_t, uint64_t&)> slice;
  std::function<Counters()> count;
  std::function<double(std::string_view)> notify;
};

template <class F> bool withEngine(const std::string& name, Data& d, const Config& c, F&& f);

Holder makeHolder(const std::string& name, Data& d, const Config& c) {
  Holder h;
  const bool ok = withEngine(name, d, c, [&](auto& e) {
    using E = std::decay_t<decltype(e)>;
    auto p = std::make_shared<E>(std::move(e));
    h.keep = p;
    Data* dp = &d;
    h.slice = [p, dp](size_t s, size_t t, uint64_t& chk) {
      const auto b = std::chrono::steady_clock::now();
      for (size_t i = s; i < t; ++i) {
        const auto r = doLookup(*p, i, dp->q[i]);
        chk = chk * 1315423911ULL + static_cast<uint64_t>(r.face + 2) * (r.depth + 1);
      }
      const auto f = std::chrono::steady_clock::now();
      return std::chrono::duration<double, std::nano>(f - b).count();
    };
    h.count = [p] { return counters(*p); };
    h.notify = [p](std::string_view x) { return notifyUpdate(*p, x); };
  });
  if (!ok) throw std::runtime_error("unknown engine " + name);
  return h;
}

std::vector<RunOut> runInterleaved(Data& d, const Config& c, size_t run, Updater* up) {
  std::vector<Holder> hs;
  for (const auto& name : c.engines) hs.push_back(makeHolder(name, d, c));
  std::vector<RunOut> out(hs.size());
  const size_t n = d.q.size();
  std::vector<size_t> order(hs.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::mt19937_64 rng(c.seed * 7919 + run);
  uint64_t chk = 0;
  std::vector<double> extra(hs.size(), 0.0);
  for (size_t s = 0; s < n; s += c.window) {
    const size_t t = std::min(n, s + c.window);
    std::fill(extra.begin(), extra.end(), 0.0);
    if (up && c.updateEvery && s && s % c.updateEvery == 0) {
      const std::string x(up->step(s));  // FIB changed once; every engine is notified
      for (size_t k = 0; k < hs.size(); ++k) extra[k] = hs[k].notify(x);
    }
    std::shuffle(order.begin(), order.end(), rng);
    for (size_t k : order) {
      out[k].windowNs.push_back(hs[k].slice(s, t, chk) + extra[k]);
      out[k].windowCounters.push_back(hs[k].count());
    }
  }
  g_sink += chk;
  for (size_t k = 0; k < hs.size(); ++k) {
    out[k].total = hs[k].count();
    out[k].total.fibProbes = 0;  // shared FIB counters are not attributable per engine here
  }
  return out;
}

double meanOver(const std::vector<double>& w, size_t window, size_t from, size_t to) {
  double ns = 0; size_t cnt = 0;
  for (size_t i = 0; i < w.size(); ++i) {
    const size_t s = i * window;
    if (s >= from && s < to) { ns += w[i]; cnt += window; }
  }
  return cnt ? ns / cnt : 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Config c;
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      auto need = [&] { if (++i >= argc) throw std::runtime_error("missing value for " + a); return std::string(argv[i]); };
      if (a == "--fib") c.fib = need();
      else if (a == "--queries") c.queries = need();
      else if (a == "--engines") c.engines = splitList(need());
      else if (a == "--runs") c.runs = std::stoul(need());
      else if (a == "--seed") c.seed = std::stoull(need());
      else if (a == "--cache") c.cache = std::stoul(need());
      else if (a == "--L") c.L = std::stoull(need());
      else if (a == "--window") c.window = std::stoul(need());
      else if (a == "--segments") { auto v = splitList(need()); c.segFirst = std::stoul(v.at(0)); c.segLast = std::stoul(v.at(1)); }
      else if (a == "--update-every") c.updateEvery = std::stoul(need());
      else if (a == "--two-stage-M") c.twoStageM = std::stoi(need());
      else if (a == "--mode") c.mode = need();
      else if (a == "--no-trie") c.noTrie = true;
      else if (a == "--no-warmup") c.warmup = false;
      else if (a == "--update-target") c.hotUpdates = need() == "hot";
      else if (a == "--tag") { c.tag = need(); std::replace(c.tag.begin(), c.tag.end(), ',', ';'); }
      else if (a == "--out") c.out = need();
      else throw std::runtime_error("unknown option " + a);
    }
    if (c.fib.empty() || c.queries.empty() || c.engines.empty()) throw std::runtime_error("--fib --queries --engines required");
    if (c.updateEvery && c.updateEvery % c.window) throw std::runtime_error("--update-every must be a multiple of --window");
    bool needV2 = false;
    for (auto& e : c.engines) if (e.rfind("v2_", 0) == 0) needV2 = true;
    Data d;
    load(c, d, needV2);
    const bool tail = c.mode == "tail";
    if (tail) calibrateTsc();
    const size_t n = d.q.size();
    std::cerr << "FIB=" << d.fibEntries.size() << " queries=" << n << " mode=" << c.mode << " M=" << d.two.startDepth() << "\n";

    std::ofstream runs(c.out + "_runs.csv");
    runs << "tag,seed,run,order,engine,cache,L,queries,mean_ns,first_ns,last_ns,hits,slow,hit_ratio,guard_rejects,"
            "resident_fallbacks,admissions,evictions,victim_rejects,filter_pos,false_misses,entries,errors,"
            "p50_ns,p90_ns,p99_ns,p999_ns,p99_last_ns,state_bytes,fib_trie_bytes,fib_hash_bytes,fib_2s_bytes,guard_index_bytes,fib_probes,bypass_lookups,guard_refreshes,invalidations\n";
    std::ofstream wins;
    if (c.mode == "timeline" || c.mode == "interleave") {
      wins.open(c.out + "_windows.csv");
      wins << "tag,seed,run,engine,window,start,end,latency_ns,hits,slow,entries,evictions\n";
    }
    Updater up; up.d = &d; up.hot = c.hotUpdates;
    Updater* upp = c.updateEvery ? &up : nullptr;

    // Untimed warm-up: every engine processes the first lookups once, so allocator,
    // page-fault and code-cache warm-up do not land on whichever engine runs first.
    if (c.warmup) {
      Data* dp = &d;
      const size_t keep = dp->q.size();
      const size_t w = std::min<size_t>(keep, 30000);
      for (const auto& name : c.engines) {
        withEngine(name, d, c, [&](auto& e) {
          uint64_t chk = 0;
          for (size_t i = 0; i < w; ++i) chk += static_cast<uint64_t>(doLookup(e, i, d.q[i]).face + 2);
          g_sink += chk;
        });
      }
    }
    for (size_t run = 1; run <= c.runs; ++run) {
      std::vector<RunOut> inter;
      if (c.mode == "interleave") {
        if (upp) up.reset(c.seed * 1000 + run);
        inter = runInterleaved(d, c, run, upp);
        if (upp) up.finish();
      }
      std::vector<size_t> order(c.engines.size());
      for (size_t i = 0; i < order.size(); ++i) order[i] = i;
      std::mt19937_64 rng(c.seed ^ (run * 0x9e3779b97f4a7c15ULL));
      std::shuffle(order.begin(), order.end(), rng);
      size_t pos = 0;
      for (size_t oi : order) {
        const std::string& name = c.engines[oi];
        RunOut ro;
        if (upp && c.mode != "interleave") up.reset(c.seed * 1000 + run);
        const int64_t before = liveBytes();
        if (c.mode == "interleave") {
          ro = std::move(inter[oi]);
        } else {
          const bool ok = withEngine(name, d, c, [&](auto& e) {
            ro = runEngine(e, d, c, tail, upp);
            ro.stateBytes = liveBytes() - before;
          });
          if (!ok) throw std::runtime_error("unknown engine " + name);
        }
        if (upp && c.mode != "interleave") up.finish();
        if (upp) up.reset(c.seed * 1000 + run);
        withEngine(name, d, c, [&](auto& e) { ro.errors = validate(e, d, c, upp); });
        if (upp) { auto x = up.finish(); (void)x; }
        ++pos;
        double p50 = 0, p90 = 0, p99 = 0, p999 = 0, p99l = 0, mean = 0, first = 0, last = 0;
        if (tail) {
          std::vector<double> v(ro.ticks.size());
          for (size_t i = 0; i < v.size(); ++i) v[i] = std::max(0.0, (ro.ticks[i] - g_tscOverhead) * g_tscNs);
          double s = 0; for (double x : v) s += x;
          mean = s / v.size();
          std::vector<double> lastPart(v.end() - std::min(v.size(), c.segLast), v.end());
          auto pct = [](std::vector<double> x, double p) {
            const size_t k = std::min(x.size() - 1, static_cast<size_t>(p * (x.size() - 1)));
            std::nth_element(x.begin(), x.begin() + k, x.end());
            return x[k];
          };
          p50 = pct(v, .5); p90 = pct(v, .9); p99 = pct(v, .99); p999 = pct(v, .999); p99l = pct(lastPart, .99);
          // hit/miss split over the last segment
          double sh = 0, sm = 0; size_t nh = 0, nm = 0;
          for (size_t i = n - std::min(n, c.segLast); i < n; ++i) {
            if (ro.hitFlag[i]) { sh += v[i]; ++nh; } else { sm += v[i]; ++nm; }
          }
          first = nh ? sh / nh : 0;  // reported in first_ns column in tail mode: mean hit cost (last segment)
          last = nm ? sm / nm : 0;   // reported in last_ns column in tail mode: mean miss cost (last segment)
        } else {
          double s = 0; for (double x : ro.windowNs) s += x;
          mean = s / n;
          first = meanOver(ro.windowNs, c.window, 0, c.segFirst);
          last = meanOver(ro.windowNs, c.window, n - std::min(n, c.segLast), n);
          if (wins) {
            Counters prev;
            for (size_t w = 0; w < ro.windowNs.size(); ++w) {
              const Counters& k = ro.windowCounters[w];
              const size_t s0 = w * c.window, s1 = std::min(n, s0 + c.window);
              wins << c.tag << ',' << c.seed << ',' << run << ',' << name << ',' << w + 1 << ',' << s0 + 1 << ',' << s1
                   << ',' << ro.windowNs[w] / (s1 - s0) << ',' << (k.hits - prev.hits) << ',' << (k.slow - prev.slow)
                   << ',' << k.entries << ',' << (k.evictions - prev.evictions) << '\n';
              prev = k;
            }
          }
        }
        const Counters& t = ro.total;
        runs << c.tag << ',' << c.seed << ',' << run << ',' << pos << ',' << name << ',' << c.cache << ',' << c.L << ','
             << n << ',' << std::fixed << std::setprecision(3) << mean << ',' << first << ',' << last << ',' << t.hits
             << ',' << t.slow << ',' << (t.cached ? static_cast<double>(t.hits) / n : 0.0) << ',' << t.guardRejects
             << ',' << t.residentFallbacks << ',' << t.admissions << ',' << t.evictions << ',' << t.victimRejects
             << ',' << t.filterPositives << ',' << t.falseMisses << ',' << t.entries << ',' << ro.errors << ',' << p50
             << ',' << p90 << ',' << p99 << ',' << p999 << ',' << p99l << ',' << ro.stateBytes << ',' << d.bytesTrie
             << ',' << d.bytesHash << ',' << d.bytesTwo << ',' << d.bytesGuard << ',' << t.fibProbes << ',' << t.bypassLookups << ',' << t.guardRefreshes << ',' << t.invalidations << '\n';
        std::cerr << "run " << run << " " << name << " mean=" << mean << "ns hit=" << (t.cached ? double(t.hits) / n : 0)
                  << " errors=" << ro.errors << "\n";
        if (ro.errors) { std::cerr << "CORRECTNESS FAILURE\n"; return 2; }
      }
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
