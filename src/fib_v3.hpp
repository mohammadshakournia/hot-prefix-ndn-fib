// fib_v3.hpp -- Hot-prefix v3: allocation-free name handling, flat cache
// index probed longest-resident-first, exact hashed branch guards, and
// baselines (trie, hash FIB, two-stage hash FIB, Kim-style, PBC, full-name
// cache) that share the same name front end and the same exact fallback.
//
// The admission/tracking policies (B0/B1/B2) are reused unchanged from the
// v2 code base (third_party/fib_v2_common.hpp) so that admission behaviour is
// identical to the first prototype (third_party/fib_v2_common.hpp).
#pragma once

#include "../third_party/fib_v2_common.hpp"

#include <bit>
#include <cstring>
#include <functional>
#include <queue>
#include <stdexcept>
#include <string_view>
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace fibv3 {

constexpr int kMaxComps = 63;  // depth bitmask uses bits 1..63

// ---------------------------------------------------------------------------
// Hashing
// ---------------------------------------------------------------------------
inline uint64_t mix64(uint64_t k) {
  k ^= k >> 33; k *= 0xff51afd7ed558ccdULL;
  k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL;
  k ^= k >> 33; return k;
}

inline uint64_t absorbBytes(uint64_t h, const char* p, size_t n) {
  constexpr uint64_t M = 0x9E3779B97F4A7C15ULL;
  while (n >= 8) {
    uint64_t w; std::memcpy(&w, p, 8);
    h = std::rotl((h ^ w) * M, 31);
    p += 8; n -= 8;
  }
  uint64_t t = 0;
  if (n) std::memcpy(&t, p, n);
  h = std::rotl((h ^ t ^ (static_cast<uint64_t>(n) << 59)) * M, 31);
  return h;
}

constexpr uint64_t kPrefixSeed = 0x243F6A8885A308D3ULL;
constexpr uint64_t kSep = 0x13198A2E03707344ULL;
constexpr uint64_t kCompSeed = 0xA4093822299F31D0ULL;

inline uint64_t compHash(std::string_view c) {
  return mix64(absorbBytes(kCompSeed, c.data(), c.size()));
}

// ---------------------------------------------------------------------------
// Name view: component boundaries (and optionally cumulative prefix hashes)
// computed in one pass, without heap allocation. Names must be canonical:
// "/c1/c2/.../cm", no empty components, no trailing slash. The loaders
// canonicalize input outside the timed region.
// ---------------------------------------------------------------------------
struct NameView {
  const char* base = nullptr;
  int m = 0;
  uint32_t end[kMaxComps + 1];
  uint64_t ph[kMaxComps + 1];
  std::string_view prefix(int k) const { return {base, end[k]}; }
  std::string_view comp(int k) const {
    const uint32_t b = end[k - 1] + 1;
    return {base + b, end[k] - b};
  }
};

// One-pass, word-at-a-time parser. The prefix hash of length L is defined as
//   H(L) = fin(state after absorbing the floor(L/8) full 8-byte words,
//              bytes [8*floor(L/8), L) of the next word, zero padded, L),
// so it depends only on the first L bytes and equals the hash of the
// prefix string itself. Component boundaries come from a SWAR '/' mask.
inline uint64_t slashMask(uint64_t w) {
  constexpr uint64_t K = 0x2F2F2F2F2F2F2F2FULL, L = 0x7F7F7F7F7F7F7F7FULL;
  const uint64_t x = w ^ K;               // zero byte where '/'
  const uint64_t t = ((x & L) + L) | x;   // high bit set where byte != 0
  return ~t & ~L;                         // high bit set where byte == '/'
}
inline uint64_t absorbWord(uint64_t st, uint64_t w) {
  return std::rotl((st ^ w) * 0x9E3779B97F4A7C15ULL, 31);
}
inline uint64_t finPrefix(uint64_t st, uint64_t partial, uint64_t len) {
  return mix64(std::rotl((st ^ partial) * 0xC2B2AE3D27D4EB4FULL, 23) + len);
}

template <bool Hash>
inline bool parseName(std::string_view s, NameView& v) {
  v.base = s.data(); v.m = 0; v.end[0] = 0;
  const size_t n = s.size();
  if (n == 0) return true;
  if (s[0] != '/') return false;
  const char* p = s.data();
  uint64_t st = kPrefixSeed;
  size_t prevSlash = 0;
  uint64_t lastW = 0, lastSt = st;
  for (size_t o = 0; o < n; o += 8) {
    uint64_t w = 0;
    const size_t rem = n - o;
    if (rem >= 8) std::memcpy(&w, p + o, 8); else std::memcpy(&w, p + o, rem);
    uint64_t mk = slashMask(w);
    while (mk) {
      const size_t b = static_cast<size_t>(std::countr_zero(mk)) >> 3;
      mk &= mk - 1;
      const size_t pos = o + b;
      if (pos == 0) continue;
      if (pos == prevSlash + 1 || v.m == kMaxComps) return false;
      ++v.m;
      v.end[v.m] = static_cast<uint32_t>(pos);
      if constexpr (Hash) v.ph[v.m] = finPrefix(st, b ? (w & ((1ULL << (8 * b)) - 1)) : 0, pos);
      prevSlash = pos;
    }
    lastW = w; lastSt = st;
    if constexpr (Hash) st = absorbWord(st, w);
  }
  if (prevSlash == n - 1 || v.m == kMaxComps) return false;
  ++v.m;
  v.end[v.m] = static_cast<uint32_t>(n);
  if constexpr (Hash) v.ph[v.m] = (n % 8 == 0) ? finPrefix(st, 0, n) : finPrefix(lastSt, lastW, n);
  return true;
}

inline std::string canonicalize(std::string_view s) {
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && s[i] == '/') ++i;
    size_t j = i;
    while (j < s.size() && s[j] != '/' && s[j] != '\r' && s[j] != '\n') ++j;
    if (j > i) { out.push_back('/'); out.append(s.substr(i, j - i)); }
    i = j;
    while (i < s.size() && (s[i] == '\r' || s[i] == '\n')) ++i;
  }
  return out;
}

inline uint64_t prefixKey(std::string_view canon) {
  NameView v;
  if (!parseName<true>(canon, v) || v.m == 0) throw std::invalid_argument("bad prefix");
  return v.ph[v.m];
}

inline uint64_t maskUpTo(int hi) {  // depths 1..hi
  return hi >= 63 ? ~0ULL : ((1ULL << (hi + 1)) - 1);
}
inline uint64_t maskBelow(int lo) {  // depths < lo
  return lo <= 0 ? 0ULL : ((1ULL << lo) - 1);
}
inline int topDepth(uint64_t bits) { return 63 - std::countl_zero(bits); }

// ---------------------------------------------------------------------------
// Result: the prefix is implied by (query, depth), so no string is returned.
// ---------------------------------------------------------------------------
struct LpmResult {
  int32_t face = -1;
  int16_t depth = 0;
  bool matched = false;
};
inline bool sameResult(const LpmResult& a, const LpmResult& b) {
  return a.matched == b.matched && (!a.matched || (a.face == b.face && a.depth == b.depth));
}

// ---------------------------------------------------------------------------
// Flat open-addressing index (linear probing, backward-shift deletion).
// ---------------------------------------------------------------------------
class FlatIndex {
 public:
  static constexpr uint32_t kEmpty = 0xFFFFFFFFu;
  struct Slot { uint64_t key; uint32_t val; };

  void init(size_t expected) {
    size_t cap = 16;
    while (cap < expected * 2) cap <<= 1;
    s_.assign(cap, Slot{0, kEmpty});
    mask_ = cap - 1; n_ = 0;
  }
  void clear() { for (auto& x : s_) x.val = kEmpty; n_ = 0; }
  size_t size() const { return n_; }
  size_t bytes() const { return s_.capacity() * sizeof(Slot); }

  template <class Eq>
  uint32_t find(uint64_t key, Eq&& eq) const {
    size_t i = key & mask_;
    for (;;) {
      const Slot& x = s_[i];
      if (x.val == kEmpty) return kEmpty;
      if (x.key == key && eq(x.val)) return x.val;
      i = (i + 1) & mask_;
    }
  }
  void insert(uint64_t key, uint32_t val) {
    if ((n_ + 1) * 2 > s_.size()) grow();
    size_t i = key & mask_;
    while (s_[i].val != kEmpty) i = (i + 1) & mask_;
    s_[i] = {key, val}; ++n_;
  }
  template <class Eq>
  bool erase(uint64_t key, Eq&& eq) {
    size_t i = key & mask_;
    for (;;) {
      if (s_[i].val == kEmpty) return false;
      if (s_[i].key == key && eq(s_[i].val)) break;
      i = (i + 1) & mask_;
    }
    size_t j = i;
    for (;;) {
      j = (j + 1) & mask_;
      if (s_[j].val == kEmpty) break;
      const size_t h = s_[j].key & mask_;
      if (((j - h) & mask_) >= ((j - i) & mask_)) { s_[i] = s_[j]; i = j; }
    }
    s_[i].val = kEmpty; --n_;
    return true;
  }

 private:
  void grow() {
    std::vector<Slot> old;
    old.swap(s_);
    s_.assign(old.size() * 2, Slot{0, kEmpty});
    mask_ = s_.size() - 1; n_ = 0;
    for (const auto& x : old) if (x.val != kEmpty) insert(x.key, x.val);
  }
  std::vector<Slot> s_;
  size_t mask_ = 0, n_ = 0;
};

inline bool bytesEqual(const std::string& a, std::string_view b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), b.size()) == 0;
}

// ---------------------------------------------------------------------------
// Reference component trie (the paper's "trie" baseline and the authority
// for guard construction). Children are keyed by component with
// heterogeneous string_view lookup, so lookups do not allocate.
// ---------------------------------------------------------------------------
struct SvHash {
  using is_transparent = void;
  size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};

class TrieFib3 {
 public:
  struct Node {
    std::unordered_map<std::string, std::unique_ptr<Node>, SvHash, std::equal_to<>> child;
    int face = -1;
  };

  bool insert(std::string_view canon, int face) {
    NameView v;
    if (!parseName<false>(canon, v) || v.m == 0) return false;
    Node* cur = root_.get();
    for (int k = 1; k <= v.m; ++k) {
      const auto c = v.comp(k);
      auto it = cur->child.find(c);
      if (it == cur->child.end())
        it = cur->child.emplace(std::string(c), std::make_unique<Node>()).first;
      cur = it->second.get();
    }
    if (cur->face < 0) ++n_;
    cur->face = face;
    maxDepth_ = std::max(maxDepth_, v.m);
    ++rev_;
    return true;
  }
  bool remove(std::string_view canon) {
    Node* cur = const_cast<Node*>(findNode(canon));
    if (!cur || cur->face < 0) return false;
    cur->face = -1; --n_; ++rev_;
    return true;
  }

  LpmResult lookupParsed(const NameView& q) const {
    LpmResult best;
    const Node* cur = root_.get();
    for (int k = 1; k <= q.m; ++k) {
      auto it = cur->child.find(q.comp(k));
      if (it == cur->child.end()) break;
      cur = it->second.get();
      if (cur->face >= 0) best = {cur->face, static_cast<int16_t>(k), true};
    }
    return best;
  }
  LpmResult lookup(std::string_view name) const {
    NameView q;
    if (!parseName<false>(name, q)) return {};
    return lookupParsed(q);
  }

  const Node* findNode(std::string_view canon) const {
    NameView v;
    if (!parseName<false>(canon, v)) return nullptr;
    const Node* cur = root_.get();
    for (int k = 1; k <= v.m; ++k) {
      auto it = cur->child.find(v.comp(k));
      if (it == cur->child.end()) return nullptr;
      cur = it->second.get();
    }
    return cur;
  }
  static bool hasRegistered(const Node* n) {
    if (n->face >= 0) return true;
    for (const auto& kv : n->child) if (hasRegistered(kv.second.get())) return true;
    return false;
  }
  // G(p): next components under which a registered descendant exists.
  std::vector<std::string> guardComponents(std::string_view canon) const {
    std::vector<std::string> out;
    const Node* n = findNode(canon);
    if (!n) return out;
    for (const auto& kv : n->child) if (hasRegistered(kv.second.get())) out.push_back(kv.first);
    return out;
  }
  std::vector<std::string> childComponents(std::string_view canon) const {
    std::vector<std::string> out;
    const Node* n = findNode(canon);
    if (!n) return out;
    out.reserve(n->child.size());
    for (const auto& kv : n->child) out.push_back(kv.first);
    return out;
  }
  uint64_t revision() const { return rev_; }
  size_t size() const { return n_; }
  int maxDepth() const { return maxDepth_; }

 private:
  std::unique_ptr<Node> root_ = std::make_unique<Node>();
  uint64_t rev_ = 0;
  size_t n_ = 0;
  int maxDepth_ = 0;
};

// ---------------------------------------------------------------------------
// Static guard index: G(p) for every registered prefix that has registered
// descendants, built directly from the FIB entries (no trie). For a FIB built
// by insertions only, the immediate children used by PBC equal G(p).
// ---------------------------------------------------------------------------
class GuardIndex {
 public:
  void build(const std::vector<std::pair<std::string, int>>& entries) {
    std::unordered_set<std::string_view> reg;
    reg.reserve(entries.size() * 2);
    for (const auto& e : entries) reg.insert(e.first);
    std::unordered_map<std::string_view, std::unordered_set<std::string_view>> tmp;
    for (const auto& e : entries) {
      NameView v;
      if (!parseName<false>(e.first, v)) continue;
      for (int k = 1; k < v.m; ++k) {
        const auto a = v.prefix(k);
        if (reg.count(a)) tmp[a].insert(v.comp(k + 1));
      }
    }
    sets_.clear();
    idx_.init(tmp.size() + 1);
    for (auto& [a, cs] : tmp) {
      std::vector<std::string> v;
      v.reserve(cs.size());
      for (auto c : cs) v.emplace_back(c);
      std::sort(v.begin(), v.end());
      sets_.emplace_back(std::string(a), std::move(v));
      idx_.insert(prefixKey(a), static_cast<uint32_t>(sets_.size() - 1));
    }
  }
  std::vector<std::string> guardComponents(std::string_view p) const {
    const uint32_t r = idx_.find(prefixKey(p), [&](uint32_t v) { return bytesEqual(sets_[v].first, p); });
    return r == FlatIndex::kEmpty ? std::vector<std::string>{} : sets_[r].second;
  }
  std::vector<std::string> childComponents(std::string_view p) const { return guardComponents(p); }
  uint64_t revision() const { return 1; }
  size_t withDescendants() const { return sets_.size(); }

 private:
  std::vector<std::pair<std::string, std::vector<std::string>>> sets_;
  FlatIndex idx_;
};

// ---------------------------------------------------------------------------
// Hash FIB: one flat table of all prefixes keyed by cumulative prefix hash,
// probed longest-first over the occupied depths (the paper's "adaptive hash").
// ---------------------------------------------------------------------------
class HashFib3 {
 public:
  struct Rec { std::string prefix; int32_t face; int16_t depth; };

  void insert(std::string_view canon, int face) {
    const uint64_t key = prefixKey(canon);
    const uint32_t r = findRec(key, canon);
    if (r != FlatIndex::kEmpty) { recs_[r].face = face; ++rev_; return; }
    NameView v; parseName<false>(canon, v);
    recs_.push_back({std::string(canon), face, static_cast<int16_t>(v.m)});
    idx_.insert(key, static_cast<uint32_t>(recs_.size() - 1));
    if (++depthCount_[v.m] == 1) depthMask_ |= 1ULL << v.m;
    ++rev_;
  }
  bool remove(std::string_view canon) {
    const uint64_t key = prefixKey(canon);
    const uint32_t r = findRec(key, canon);
    if (r == FlatIndex::kEmpty || recs_[r].face < 0) return false;
    const int d = recs_[r].depth;
    idx_.erase(key, [&](uint32_t v) { return v == r; });
    recs_[r].face = -1;  // tombstoned record (index entry removed)
    if (--depthCount_[d] == 0) depthMask_ &= ~(1ULL << d);
    ++rev_;
    return true;
  }
  void reserve(size_t n) { idx_.init(n); recs_.reserve(n); }

  // LPM restricted to depths in [lo, hi].
  LpmResult resolveRange(const NameView& q, int hi, int lo) const {
    uint64_t bits = depthMask_ & maskUpTo(hi) & ~maskBelow(lo);
    while (bits) {
      const int d = topDepth(bits);
      bits &= ~(1ULL << d);
      ++probes_;
      const auto sv = q.prefix(d);
      const uint32_t r = idx_.find(q.ph[d], [&](uint32_t v) { return bytesEqual(recs_[v].prefix, sv); });
      if (r != FlatIndex::kEmpty) return {recs_[r].face, static_cast<int16_t>(d), true};
    }
    return {};
  }
  LpmResult resolve(const NameView& q) const { return resolveRange(q, q.m, 1); }
  LpmResult resolveAbove(const NameView& q, int d) const { return resolveRange(q, q.m, d + 1); }
  LpmResult lookup(std::string_view name) const {
    NameView q;
    if (!parseName<true>(name, q)) return {};
    return resolve(q);
  }
  uint64_t revision() const { return rev_; }
  uint64_t probes() const { return probes_; }
  void resetProbes() const { probes_ = 0; }

 private:
  uint32_t findRec(uint64_t key, std::string_view canon) const {
    return idx_.find(key, [&](uint32_t v) { return bytesEqual(recs_[v].prefix, canon); });
  }
  std::vector<Rec> recs_;
  FlatIndex idx_ = [] { FlatIndex f; f.init(16); return f; }();
  int depthCount_[kMaxComps + 1] = {};
  uint64_t depthMask_ = 0;
  uint64_t rev_ = 0;
  mutable uint64_t probes_ = 0;
};

// ---------------------------------------------------------------------------
// Two-stage hash FIB (So et al., ANCS 2013; the scheme used by NDN-DPDK):
// a probe at a fixed start depth M hits either a real prefix or a virtual
// marker that records the maximum registered depth below it, which bounds
// the subsequent longest-first probes. Static build only.
// ---------------------------------------------------------------------------
class HashFib2Stage {
 public:
  struct Rec { std::string prefix; int32_t face; int16_t depth; int16_t maxBelow; bool real; };

  // entries: canonical prefixes and faces; M <= 0 selects the median depth.
  void build(const std::vector<std::pair<std::string, int>>& entries, int M = 0) {
    std::vector<int> depths;
    depths.reserve(entries.size());
    for (const auto& e : entries) { NameView v; parseName<false>(e.first, v); depths.push_back(v.m); }
    if (M <= 0) {
      auto tmp = depths;
      std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
      M = tmp[tmp.size() / 2];
    }
    M_ = M;
    idx_.init(entries.size() * 2);
    for (size_t i = 0; i < entries.size(); ++i) {
      const std::string& p = entries[i].first;
      NameView v; parseName<true>(p, v);
      const int d = v.m;
      uint32_t r = find(v.ph[d], v.prefix(d));
      if (r == FlatIndex::kEmpty) {
        recs_.push_back({p, entries[i].second, static_cast<int16_t>(d), static_cast<int16_t>(d), true});
        r = static_cast<uint32_t>(recs_.size() - 1);
        idx_.insert(v.ph[d], r);
      } else {
        recs_[r].real = true; recs_[r].face = entries[i].second;
        recs_[r].maxBelow = std::max<int16_t>(recs_[r].maxBelow, static_cast<int16_t>(d));
      }
      depthMask_ |= 1ULL << d;
      if (d > M_) {
        const auto pm = v.prefix(M_);
        uint32_t m = find(v.ph[M_], pm);
        if (m == FlatIndex::kEmpty) {
          recs_.push_back({std::string(pm), -1, static_cast<int16_t>(M_), static_cast<int16_t>(d), false});
          idx_.insert(v.ph[M_], static_cast<uint32_t>(recs_.size() - 1));
        } else {
          recs_[m].maxBelow = std::max<int16_t>(recs_[m].maxBelow, static_cast<int16_t>(d));
        }
      }
    }
  }

  LpmResult resolve(const NameView& q) const {
    if (q.m >= M_) {
      ++probes_;
      const uint32_t r = find(q.ph[M_], q.prefix(M_));
      if (r != FlatIndex::kEmpty) {
        const Rec& x = recs_[r];
        const int hi = std::min<int>(q.m, x.maxBelow);
        if (hi > M_) {
          LpmResult deep = probeReal(q, hi, M_ + 1);
          if (deep.matched) return deep;
        }
        if (x.real) return {x.face, static_cast<int16_t>(M_), true};
      }
      return probeReal(q, M_ - 1, 1);
    }
    return probeReal(q, q.m, 1);
  }
  LpmResult resolveAbove(const NameView& q, int d) const { return probeReal(q, q.m, d + 1); }
  LpmResult lookup(std::string_view name) const {
    NameView q;
    if (!parseName<true>(name, q)) return {};
    return resolve(q);
  }
  int startDepth() const { return M_; }
  uint64_t probes() const { return probes_; }
  void resetProbes() const { probes_ = 0; }

 private:
  uint32_t find(uint64_t key, std::string_view sv) const {
    return idx_.find(key, [&](uint32_t v) { return bytesEqual(recs_[v].prefix, sv); });
  }
  LpmResult probeReal(const NameView& q, int hi, int lo) const {
    uint64_t bits = depthMask_ & maskUpTo(hi) & ~maskBelow(lo);
    while (bits) {
      const int d = topDepth(bits);
      bits &= ~(1ULL << d);
      ++probes_;
      const uint32_t r = find(q.ph[d], q.prefix(d));
      if (r != FlatIndex::kEmpty && recs_[r].real) return {recs_[r].face, static_cast<int16_t>(d), true};
    }
    return {};
  }
  std::vector<Rec> recs_;
  FlatIndex idx_;
  uint64_t depthMask_ = 0;
  int M_ = 1;
  mutable uint64_t probes_ = 0;
};

// ---------------------------------------------------------------------------
// Admit-all ablation policy: every resolved non-resident prefix is admitted
// and always replaces the sampled CLOCK victim (no frequency filter).
// ---------------------------------------------------------------------------
class AdmitAllPolicy {
 public:
  AdmitAllPolicy(size_t, double) {}
  double estimate(const std::string&, uint64_t) const { return 0.0; }
  double observe(const std::string&, uint64_t) { return 1e300; }
  void checkpoint(const std::string&, double, uint64_t) {}
  bool eligibleForAdmission(const std::string&, double) const { return true; }
  bool beatsVictim(const std::string&, double, double, uint64_t) { return true; }
  void onPromotion(const std::string&) {}
  void clear() {}
  void resetStats() {}
};

// ---------------------------------------------------------------------------
// B2 keyed by the 64-bit prefix hash (B2h). Same algorithm as the v2
// BoundedFrequencySketch (doorkeeper, conservative update, 8-bit saturating
// counters, observation-count aging, approximate retirement on promotion,
// bounded checkpoint raise), but row slots are derived from the prefix hash
// computed during name parsing, so a miss performs no string hashing or
// allocation. Collisions differ from v2, so traces are not bit-identical.
// ---------------------------------------------------------------------------
class SketchPolicyH {
 public:
  struct Config { size_t depth = 2; size_t width = 2048; uint64_t agingWindow = 8192; bool retire = true; };
  explicit SketchPolicyH(Config c) : depth_(std::max<size_t>(1, std::min<size_t>(4, c.depth))), aging_(c.agingWindow), retire_(c.retire) {
    width_ = 1; while (width_ < c.width) width_ <<= 1;
    counters_.assign(depth_ * width_, 0);
    door_.assign(width_ / 64 + 1, 0);
  }
  double estimateKey(uint64_t k) const {
    if (!inDoor(k)) return 0.0;
    return 1.0 + cms(k);
  }
  double observeKey(uint64_t k) {
    ++obs_;
    if (aging_ && obs_ % aging_ == 0) {
      for (auto& c : counters_) c >>= 1;
      std::fill(door_.begin(), door_.end(), 0);
      ++agings_;
    }
    if (!inDoor(k)) { setDoor(k); return 1.0 + cms(k); }
    inc(k);
    return 1.0 + cms(k);
  }
  void checkpointKey(uint64_t k, double value) {
    if (!std::isfinite(value) || value <= 0.0) return;
    if (!inDoor(k)) setDoor(k);
    const double target = std::min(value, 254.0);
    for (size_t s = 0; s < 64 && estimateKey(k) < target; ++s) inc(k);
  }
  void onPromotionKey(uint64_t k) {
    if (!retire_) return;
    const uint8_t a = cms(k);
    if (!a) return;
    for (size_t r = 0; r < depth_; ++r) { uint8_t& c = counters_[r * width_ + slot(k, r)]; c = c > a ? c - a : 0; }
  }
  bool beats(double est, double victim) const { return est > victim; }
  void clear() { std::fill(counters_.begin(), counters_.end(), 0); std::fill(door_.begin(), door_.end(), 0); obs_ = 0; agings_ = 0; }
  size_t logicalBytes() const { return counters_.size() + door_.size() * 8; }
  uint64_t agingCount() const { return agings_; }

 private:
  static constexpr uint64_t kSeeds[4] = {0x9E3779B97F4A7C15ULL, 0xC2B2AE3D27D4EB4FULL, 0x165667B19E3779F9ULL, 0x27D4EB2F165667C5ULL};
  size_t slot(uint64_t k, size_t r) const { return static_cast<size_t>(mix64(k ^ kSeeds[r])) & (width_ - 1); }
  bool inDoor(uint64_t k) const { const size_t b = static_cast<size_t>(k >> 7) & (width_ - 1); return (door_[b >> 6] >> (b & 63)) & 1; }
  void setDoor(uint64_t k) { const size_t b = static_cast<size_t>(k >> 7) & (width_ - 1); door_[b >> 6] |= 1ULL << (b & 63); }
  uint8_t cms(uint64_t k) const {
    uint8_t m = 255;
    for (size_t r = 0; r < depth_; ++r) m = std::min(m, counters_[r * width_ + slot(k, r)]);
    return m;
  }
  void inc(uint64_t k) {
    size_t idx[4];
    uint8_t m = 255;
    for (size_t r = 0; r < depth_; ++r) { idx[r] = r * width_ + slot(k, r); m = std::min(m, counters_[idx[r]]); }
    if (m >= 255) return;
    for (size_t r = 0; r < depth_; ++r) if (counters_[idx[r]] == m) ++counters_[idx[r]];
  }
  size_t depth_, width_ = 1;
  uint64_t aging_, obs_ = 0, agings_ = 0;
  bool retire_;
  std::vector<uint8_t> counters_;
  std::vector<uint64_t> door_;
};

// Exact guard set: hashes sorted with their component strings.
struct Guard {
  std::vector<uint64_t> h;
  std::vector<std::string> s;
  void build(std::vector<std::string> comps) {
    std::vector<std::pair<uint64_t, std::string>> t;
    t.reserve(comps.size());
    for (auto& c : comps) t.emplace_back(compHash(c), std::move(c));
    std::sort(t.begin(), t.end());
    h.clear(); s.clear(); h.reserve(t.size()); s.reserve(t.size());
    for (auto& x : t) { h.push_back(x.first); s.push_back(std::move(x.second)); }
  }
  bool contains(std::string_view c) const {
    if (h.empty()) return false;
    const uint64_t x = compHash(c);
    size_t i;
    if (h.size() <= 8) {
      for (i = 0; i < h.size(); ++i) if (h[i] == x && bytesEqual(s[i], c)) return true;
      return false;
    }
    i = static_cast<size_t>(std::lower_bound(h.begin(), h.end(), x) - h.begin());
    for (; i < h.size() && h[i] == x; ++i) if (bytesEqual(s[i], c)) return true;
    return false;
  }
  size_t bytes() const {
    size_t b = h.capacity() * sizeof(uint64_t) + s.capacity() * sizeof(std::string);
    for (const auto& x : s) if (x.capacity() > 15) b += x.capacity() + 1;
    return b;
  }
};

struct HotConfig {
  size_t capacity = 512;
  double promote = 4.0;
  double alpha = 1.0;
  double cool = 0.0;
  uint64_t learningLimit = 65536;  // 0: no learning cutoff
  uint32_t sampleEvery = 1;        // track only every k-th eligible miss (1 = all)
  bool lowerBound = true;          // resume the fallback below a rejected resident prefix
  // Stage B (all off by default, so earlier configurations are unchanged)
  double thetaFree = 0.0;          // admission threshold while free slots exist (0: use promote)
  bool scaleBySample = false;      // candidate estimates x sampleEvery when compared with residents
  bool sharedAging = false;        // resident scores halve whenever the sketch ages (alpha = 1)
  bool adaptive = false;           // adaptive mode controller (cache on / bypass)
  double margin = 0.03;            // switch when the other mode is faster by more than this
  uint32_t exploreEvery = 4;       // epochs in the current mode between explorations
  uint32_t bypassSampleEvery = 0;  // tracking rate on the bypass path (0: none, pure FIB lookup)
  bool forceBypass = false;        // diagnostics: stay in bypass, never explore
  // Stage C: incremental maintenance. The FIB owner calls onFibUpdate(x) after
  // every change of prefix x; the cache is then never flushed. Without a
  // notification a revision change still clears the cache (safety net).
  bool incremental = false;
  uint64_t epochLen = 8192;        // lookups per controller epoch (power of two)
};

// ---------------------------------------------------------------------------
// Hot-prefix v3. Resident prefixes are kept in a flat index keyed by the
// cumulative prefix hash, with a bitmask of resident depths. A lookup probes
// the resident depths from the longest down; the first resident prefix found
// is the longest resident prefix of the query and, by Lemma 2, the only one
// that can pass the guard test. Admission, CLOCK replacement and the
// candidate policies follow the v2 implementation exactly.
// ---------------------------------------------------------------------------
template <class Policy, class Fallback, class G = TrieFib3>
class HotPrefix3 {
  static constexpr bool kKeyed = requires(Policy p, uint64_t k) { p.observeKey(k); };
  struct Entry {
    std::string prefix;
    uint64_t key = 0;
    int depth = 0;
    int face = -1;
    Guard guard;
    double score = 0.0;
    uint64_t tick = 0;
    uint64_t agingEpoch = 0;
    bool referenced = false;
    bool used = false;
  };

 public:
  HotPrefix3(const G& fib, const Fallback& fb, Policy policy, HotConfig cfg)
      : fib_(fib), fb_(fb), cfg_(cfg), policy_(std::move(policy)), seenRevision_(fib.revision()) {
    if (!(cfg_.alpha > 0.0 && cfg_.alpha <= 1.0)) throw std::invalid_argument("alpha");
    if (cfg_.epochLen == 0 || (cfg_.epochLen & (cfg_.epochLen - 1))) throw std::invalid_argument("epochLen");
    if (cfg_.forceBypass) { modeOn_ = false; cfg_.exploreEvery = 0xFFFFFFFFu; }
    slots_.reserve(cfg_.capacity);
    idx_.init(cfg_.capacity * 2 + 1);
  }

  LpmResult lookup(std::string_view name) {
    ++tick_;
    sync();
    if (!cfg_.adaptive) {
      NameView q;
      if (!parseName<true>(name, q) || q.m == 0) return {};
      return onPath(q);
    }
    // Adaptive mode selection by bracketed epoch exploration. All lookups of
    // an epoch use one mode, so the processor-cache state reflects that mode.
    // Every 32nd lookup is timed as a whole. After every exploreEvery epochs in
    // the current mode, one epoch runs in the other mode and is compared with
    // the mean of the epochs immediately before and after it; two consecutive
    // wins by more than the margin switch the mode. Results are exact either way.
    ++ctl_;
    // time only the second half of each epoch: the first half re-warms the
    // processor caches after a possible mode change
    const bool timed = (ctl_ & 31) == 0 && (ctl_ & (cfg_.epochLen >> 1)) != 0;
    const uint64_t t0 = timed ? cycles() : 0;
    NameView q;
    LpmResult r;
    if (parseName<true>(name, q) && q.m != 0) r = (modeOn_ != exploring_) ? onPath(q) : bypassPath(q);
    if (timed) { sum_ += static_cast<double>(std::min<uint64_t>(cycles() - t0, 20000)); ++cnt_; }
    if ((ctl_ & (cfg_.epochLen - 1)) == 0) endEpoch();
    return r;
  }

  // Incremental maintenance after the FIB has applied an update of prefix x
  // (insert, remove or face change). Restores I1-I3:
  //   x resident and no longer registered -> evicted (I1);
  //   x resident and registered           -> face refreshed (I2);
  //   resident proper ancestors a of x     -> G(a) recomputed (I3).
  // No other entry is affected: G(p) depends only on p's registered
  // descendants, and a cached answer depends only on (p, f(p), G(p)).
  void onFibUpdate(std::string_view prefix) {
    if constexpr (requires(const NameView& v) { fb_.resolveRange(v, 1, 1); }) {
      onFibUpdateImpl(prefix);
    } else {
      (void)prefix;  // static fallback (two-stage FIB): updates unsupported, the revision check clears
    }
  }

 private:
  void onFibUpdateImpl(std::string_view prefix) {
    if (!cfg_.incremental) return;
    seenRevision_ = fib_.revision();
    NameView x;
    if (!parseName<true>(prefix, x) || x.m == 0) return;
    ++updates_;
    if (const uint32_t s = findResident(x, x.m); s != FlatIndex::kEmpty) {
      const LpmResult r = fb_.resolveRange(x, x.m, x.m);
#ifndef FIBV3_MUTATE_SKIP_EVICT
      if (!r.matched) { eraseSlot(s, true); ++invalidations_; }
#else
      if (!r.matched) { ++invalidations_; }
#endif
      else { slots_[s].face = r.face; ++faceRefreshes_; }
    }
    uint64_t bits = depthMask_ & maskUpTo(x.m - 1);
    while (bits) {
      const int d = topDepth(bits);
      bits &= ~(1ULL << d);
      const uint32_t s = findResident(x, d);
      if (s == FlatIndex::kEmpty) continue;
#ifndef FIBV3_MUTATE_SKIP_GUARD  // mutation hook for the tests (never defined in normal builds)
      slots_[s].guard.build(fib_.guardComponents(x.prefix(d)));
#endif
      ++guardRefreshes_;
    }
  }

 public:
  uint64_t updates() const { return updates_; }
  uint64_t invalidations() const { return invalidations_; }
  uint64_t guardRefreshes() const { return guardRefreshes_; }
  // Test helper: number of resident entries that violate I1-I3 for the current FIB.
  size_t invariantViolations() const {
    size_t bad = 0;
    if constexpr (!requires(const NameView& v) { fb_.resolveRange(v, 1, 1); }) return 0;
    else {
    for (const auto& e : slots_) {
      if (!e.used) continue;
      NameView v;
      parseName<true>(e.prefix, v);
      const LpmResult r = fb_.resolveRange(v, v.m, v.m);
      if (!r.matched || r.face != e.face) { ++bad; continue; }
      auto want = fib_.guardComponents(e.prefix);
      std::sort(want.begin(), want.end());
      auto have = e.guard.s;
      std::sort(have.begin(), have.end());
      if (want != have) ++bad;
    }
    return bad;
    }
  }

  uint64_t bypassLookups() const { return bypassLookups_; }
  uint64_t bypassEpochs() const { return bypassEpochs_; }
  uint64_t trials() const { return trials_; }
  bool inBypass() const { return !modeOn_; }
  uint64_t switches() const { return switches_; }
  double lastExplore() const { return lastExp_; }
  double lastBracket() const { return lastBracket_; }

 private:
  static uint64_t cycles() {
#if defined(__x86_64__) || defined(__i386__)
    return __rdtsc();
#else
    return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
  }

  void endEpoch() {
    ++epochs_;
    const double T = cnt_ ? sum_ / static_cast<double>(cnt_) : 0.0;
    sum_ = 0; cnt_ = 0;
    if (!modeOn_ && !exploring_) ++bypassEpochs_;
    switch (phase_) {
      case 0:  // normal epochs in the current mode
        lastT_ = T;
        if (++since_ >= cfg_.exploreEvery) { phase_ = 1; exploring_ = true; preT_ = lastT_; since_ = 0; ++trials_; }
        break;
      case 1:  // exploration epoch just ended
        expT_ = T; exploring_ = false; phase_ = 2;
        break;
      default: {  // post-bracket epoch just ended
        const double bracket = 0.5 * (preT_ + T);
        if (expT_ > 0 && bracket > 0 && expT_ < bracket * (1.0 - cfg_.margin)) {
          if (++votes_ >= 2) { modeOn_ = !modeOn_; votes_ = 0; ++switches_; }
        } else {
          votes_ = 0;
        }
        lastExp_ = expT_; lastBracket_ = bracket;
        lastT_ = T; phase_ = 0;
      }
    }
  }

  // Cache path: longest resident prefix, guard test, exact fallback.
  LpmResult onPath(const NameView& q) {
    // lb: depth of the longest resident prefix of q when it cannot answer.
    // That prefix is registered (I1) with the stored face (I2), so the LPM
    // is at least lb deep: the fallback only has to search depths > lb.
    int lb = 0;
    int32_t lbFace = -1;
    uint64_t bits = depthMask_ & maskUpTo(q.m);
    while (bits) {
      const int d = topDepth(bits);
      bits &= ~(1ULL << d);
      const auto sv = q.prefix(d);
      const uint32_t s = idx_.find(q.ph[d], [&](uint32_t v) { return bytesEqual(slots_[v].prefix, sv); });
      if (s == FlatIndex::kEmpty) continue;
      Entry& e = slots_[s];
      const bool safe = d == q.m || !e.guard.contains(q.comp(d + 1));
      if (!safe) { ++guardRejects_; lb = d; lbFace = e.face; break; }  // Lemma 2
      if (cfg_.alpha < 1.0 && cfg_.cool > 0.0 && current(e) < cfg_.cool) {
        lb = d; lbFace = e.face;
        eraseSlot(s, true);
        break;
      }
      bump(e);
      e.referenced = true;
      ++hits_;
      return {e.face, static_cast<int16_t>(d), true};
    }

    ++slow_;
    LpmResult r;
    if (cfg_.lowerBound && lb) {
      ++lbResolves_;
      r = fb_.resolveAbove(q, lb);
      if (!r.matched) r = {lbFace, static_cast<int16_t>(lb), true};
    } else {
      r = fb_.resolve(q);
    }
    return afterResolve(q, r);
  }

  // Bypass path: exact fallback only; popularity tracking continues.
  LpmResult bypassPath(const NameView& q) {
    ++slow_;
    ++bypassLookups_;
    if (cfg_.bypassSampleEvery == 0) return fb_.resolve(q);  // pure bypass: the FIB alone
    return afterResolve(q, fb_.resolve(q), cfg_.bypassSampleEvery);
  }

  LpmResult afterResolve(const NameView& q, const LpmResult& r) { return afterResolve(q, r, cfg_.sampleEvery); }

  LpmResult afterResolve(const NameView& q, const LpmResult& r, uint32_t sampleEvery) {
    if (!r.matched) return r;
    const uint32_t rs = findResident(q, r.depth);
    if (rs != FlatIndex::kEmpty) {
      Entry& e = slots_[rs];
      bump(e);
      e.referenced = true;
      ++residentFallbacks_;
      return r;
    }
    if (cfg_.alpha == 1.0 && cfg_.learningLimit && tick_ > cfg_.learningLimit) return r;
    if (sampleEvery > 1 && (++sampleCounter_ % sampleEvery) != 0) return r;
    const double threshold =
        (cfg_.thetaFree > 0.0 && occupied_ < cfg_.capacity) ? cfg_.thetaFree : cfg_.promote;
    if constexpr (kKeyed) {
      const uint64_t key = q.ph[r.depth];
      const double value = policy_.observeKey(key);
      if (value >= threshold) admit(std::string(q.prefix(r.depth)), key, r);
    } else {
      const std::string p(q.prefix(r.depth));
      const double value = policy_.observe(p, tick_);
      if (value >= threshold && policy_.eligibleForAdmission(p, value)) admit(p, q.ph[r.depth], r);
    }
    return r;
  }

 public:
  uint64_t hits() const { return hits_; }
  uint64_t slow() const { return slow_; }
  uint64_t evictions() const { return evictions_; }
  uint64_t admissions() const { return admissions_; }
  uint64_t guardRejects() const { return guardRejects_; }
  uint64_t residentFallbacks() const { return residentFallbacks_; }
  uint64_t lbResolves() const { return lbResolves_; }
  uint64_t victimRejects() const { return victimRejects_; }
  size_t cacheSize() const { return occupied_; }
  size_t guardBytes() const {
    size_t b = 0;
    for (const auto& e : slots_) if (e.used) b += e.guard.bytes();
    return b;
  }
  size_t guardEntries() const {
    size_t b = 0;
    for (const auto& e : slots_) if (e.used) b += e.guard.h.size();
    return b;
  }
  std::vector<std::pair<std::string, int>> residents() const {
    std::vector<std::pair<std::string, int>> out;
    for (const auto& e : slots_) if (e.used) out.emplace_back(e.prefix, e.depth);
    return out;
  }
  const Policy& policy() const { return policy_; }

 private:
  uint32_t findResident(const NameView& q, int d) const {
    if (!(depthMask_ & (1ULL << d))) return FlatIndex::kEmpty;
    const auto sv = q.prefix(d);
    return idx_.find(q.ph[d], [&](uint32_t v) { return bytesEqual(slots_[v].prefix, sv); });
  }
  uint64_t agingNow() const {
    if constexpr (kKeyed) return cfg_.sharedAging ? policy_.agingCount() : 0;
    else return 0;
  }
  void normalize(Entry& e) {
    const uint64_t now = agingNow();
    if (e.agingEpoch != now) {
      e.score = std::ldexp(e.score, -static_cast<int>(std::min<uint64_t>(63, now - e.agingEpoch)));
      e.agingEpoch = now;
    }
  }
  void bump(Entry& e) {
    if (cfg_.alpha < 1.0) { touch(e); return; }
    if (cfg_.sharedAging) normalize(e);
    e.score += 1.0;
  }
  double current(const Entry& e) const {
    if (cfg_.alpha == 1.0 && cfg_.sharedAging && e.agingEpoch != agingNow())
      return std::ldexp(e.score, -static_cast<int>(std::min<uint64_t>(63, agingNow() - e.agingEpoch)));
    if (cfg_.alpha == 1.0 || e.tick == tick_) return e.score;
    const uint64_t el = tick_ - e.tick;
    if (el == 1) return e.score * cfg_.alpha;
    return e.score * std::pow(cfg_.alpha, static_cast<double>(el));
  }
  void touch(Entry& e) { e.score = current(e) + 1.0; e.tick = tick_; }

  void sync() {
    if (seenRevision_ == fib_.revision()) return;
    policy_.clear();
    slots_.clear();
    idx_.clear();
    while (!free_.empty()) free_.pop();
    for (int& c : depthCount_) c = 0;
    depthMask_ = 0; occupied_ = 0; clockHand_ = 0; tick_ = 0;
    seenRevision_ = fib_.revision();
    modeOn_ = true; exploring_ = false; phase_ = 0; since_ = votes_ = 0; sum_ = 0; cnt_ = 0;
  }

  void eraseSlot(uint32_t slot, bool cooling) {
    Entry& v = slots_[slot];
    if (!v.used) return;
    if constexpr (kKeyed) policy_.checkpointKey(v.key, current(v));
    else policy_.checkpoint(v.prefix, current(v), tick_);
    const uint32_t s = slot;
    idx_.erase(v.key, [&](uint32_t x) { return x == s; });
    if (--depthCount_[v.depth] == 0) depthMask_ &= ~(1ULL << v.depth);
    v.used = false;
    v.prefix.clear();
    v.guard = Guard{};
    --occupied_;
    ++evictions_;
    if (cooling) free_.push(slot);
  }

  size_t chooseVictim() {
    const size_t n = slots_.size();
    constexpr size_t sampleSize = 8;
    size_t best = clockHand_;
    double bestScore = std::numeric_limits<double>::infinity();
    bool foundUnreferenced = false;
    for (size_t probes = 0; probes < std::min(sampleSize, n); ++probes) {
      const size_t slot = clockHand_;
      clockHand_ = (clockHand_ + 1) % n;
      Entry& e = slots_[slot];
      if (!e.used) return slot;
      const bool was = e.referenced;
      e.referenced = false;
      const double score = current(e);
      if ((!was && (!foundUnreferenced || score < bestScore)) ||
          (was && !foundUnreferenced && score < bestScore)) {
        best = slot; bestScore = score; foundUnreferenced = !was;
      }
    }
    return best;
  }

  void admit(const std::string& p, uint64_t key, const LpmResult& r) {
    if (cfg_.capacity == 0) return;
    double candidate;
    if constexpr (kKeyed) candidate = policy_.estimateKey(key);
    else candidate = policy_.estimate(p, tick_);
    if (cfg_.scaleBySample && cfg_.sampleEvery > 1) candidate *= cfg_.sampleEvery;
    uint32_t slot;
    if (occupied_ < cfg_.capacity) {
      if (!free_.empty()) { slot = free_.top(); free_.pop(); }
      else { slot = static_cast<uint32_t>(slots_.size()); slots_.emplace_back(); }
    } else {
      slot = static_cast<uint32_t>(chooseVictim());
      Entry& victim = slots_[slot];
      const double vs = victim.used ? current(victim) : 0.0;
      bool beats;
      if constexpr (kKeyed) beats = policy_.beats(candidate, vs);
      else beats = policy_.beatsVictim(p, candidate, vs, tick_);
      if (victim.used && !beats) { ++victimRejects_; return; }
      eraseSlot(slot, false);
    }
    Entry& e = slots_[slot];
    e.prefix = p;
    e.key = key;
    e.depth = r.depth;
    e.face = r.face;
    e.guard.build(fib_.guardComponents(p));
    e.score = candidate;
    e.tick = tick_;
    e.agingEpoch = agingNow();
    e.referenced = true;
    e.used = true;
    if constexpr (kKeyed) policy_.onPromotionKey(key);
    else policy_.onPromotion(p);
    idx_.insert(key, slot);
    if (++depthCount_[r.depth] == 1) depthMask_ |= 1ULL << r.depth;
    ++occupied_;
    ++admissions_;
  }

  const G& fib_;
  const Fallback& fb_;
  HotConfig cfg_;
  Policy policy_;
  uint64_t seenRevision_;
  std::vector<Entry> slots_;
  FlatIndex idx_;
  std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<uint32_t>> free_;
  int depthCount_[kMaxComps + 1] = {};
  uint64_t depthMask_ = 0;
  size_t occupied_ = 0, clockHand_ = 0;
  uint64_t tick_ = 0, hits_ = 0, slow_ = 0, evictions_ = 0, admissions_ = 0;
  uint64_t guardRejects_ = 0, residentFallbacks_ = 0, victimRejects_ = 0;
  uint64_t sampleCounter_ = 0, lbResolves_ = 0;
  uint64_t updates_ = 0, invalidations_ = 0, guardRefreshes_ = 0, faceRefreshes_ = 0;
  // adaptive mode controller (bracketed epoch exploration)
  uint64_t ctl_ = 0, epochs_ = 0, bypassEpochs_ = 0, bypassLookups_ = 0, trials_ = 0, switches_ = 0;
  bool modeOn_ = true, exploring_ = false;
  int phase_ = 0;
  uint32_t since_ = 0, votes_ = 0;
  double sum_ = 0, lastT_ = 0, preT_ = 0, expT_ = 0, lastExp_ = 0, lastBracket_ = 0;
  uint64_t cnt_ = 0;
};

// ---------------------------------------------------------------------------
// Intrusive LRU over a fixed slot array (used by the baselines).
// ---------------------------------------------------------------------------
class LruList {
 public:
  static constexpr uint32_t kNil = 0xFFFFFFFFu;
  void resize(size_t n) { prev_.assign(n, kNil); next_.assign(n, kNil); head_ = tail_ = kNil; }
  void pushFront(uint32_t i) {
    prev_[i] = kNil; next_[i] = head_;
    if (head_ != kNil) prev_[head_] = i; else tail_ = i;
    head_ = i;
  }
  void unlink(uint32_t i) {
    if (prev_[i] != kNil) next_[prev_[i]] = next_[i]; else head_ = next_[i];
    if (next_[i] != kNil) prev_[next_[i]] = prev_[i]; else tail_ = prev_[i];
    prev_[i] = next_[i] = kNil;
  }
  void moveFront(uint32_t i) { if (head_ != i) { unlink(i); pushFront(i); } }
  uint32_t tail() const { return tail_; }
 private:
  std::vector<uint32_t> prev_, next_;
  uint32_t head_ = kNil, tail_ = kNil;
};

// Kim-style route-prefix cache (Kim et al., Appl. Sci. 2020): every resolved
// prefix is admitted, LRU replacement, a Bloom filter over G(p). Uses the same
// name front end and exact fallback as Hot-prefix.
template <class Fallback, class G = TrieFib3>
class Kim3 {
  struct Entry {
    std::string prefix; uint64_t key = 0; int depth = 0; int face = -1;
    std::vector<uint64_t> bloom; bool leaf = true; bool used = false;
  };
 public:
  Kim3(const G& fib, const Fallback& fb, size_t capacity, size_t bloomBits = 128, unsigned k = 3)
      : fib_(fib), fb_(fb), cap_(capacity), bits_(bloomBits), k_(k), seenRevision_(fib.revision()) {
    slots_.resize(cap_); lru_.resize(cap_); idx_.init(cap_ * 2 + 1);
  }
  LpmResult lookup(std::string_view name) {
    sync();
    NameView q;
    if (!parseName<true>(name, q) || q.m == 0) return {};
    uint64_t bits = depthMask_ & maskUpTo(q.m);
    while (bits) {
      const int d = topDepth(bits);
      bits &= ~(1ULL << d);
      const auto sv = q.prefix(d);
      const uint32_t s = idx_.find(q.ph[d], [&](uint32_t v) { return bytesEqual(slots_[v].prefix, sv); });
      if (s == FlatIndex::kEmpty) continue;
      Entry& e = slots_[s];
      const bool safe = d == q.m || e.leaf || !maybe(e, q.comp(d + 1));
      if (safe) { lru_.moveFront(s); ++hits_; return {e.face, static_cast<int16_t>(d), true}; }
      ++filterPositives_;
      break;
    }
    ++slow_;
    LpmResult r = fb_.resolve(q);
    admit(q, r);
    return r;
  }
  uint64_t hits() const { return hits_; }
  uint64_t slow() const { return slow_; }
  uint64_t filterPositives() const { return filterPositives_; }
  size_t cacheSize() const { return size_; }

 private:
  bool maybe(const Entry& e, std::string_view c) const {
    const uint64_t h1 = compHash(c), h2 = mix64(h1 ^ 0x5bd1e995ULL) | 1;
    for (unsigned i = 0; i < k_; ++i) {
      const uint64_t b = (h1 + i * h2) % bits_;
      if (!(e.bloom[b >> 6] & (1ULL << (b & 63)))) return false;
    }
    return true;
  }
  void sync() {
    if (seenRevision_ == fib_.revision()) return;
    for (auto& e : slots_) e = Entry{};
    lru_.resize(cap_); idx_.clear(); size_ = 0; depthMask_ = 0;
    for (int& c : depthCount_) c = 0;
    seenRevision_ = fib_.revision();
  }
  void admit(const NameView& q, const LpmResult& r) {
    if (!r.matched || cap_ == 0) return;
    const auto sv = q.prefix(r.depth);
    const uint32_t ex = idx_.find(q.ph[r.depth], [&](uint32_t v) { return bytesEqual(slots_[v].prefix, sv); });
    if (ex != FlatIndex::kEmpty) { lru_.moveFront(ex); return; }
    uint32_t s;
    if (size_ < cap_) s = static_cast<uint32_t>(size_++);
    else { s = lru_.tail(); evict(s); }
    Entry& e = slots_[s];
    e.prefix.assign(sv); e.key = q.ph[r.depth]; e.depth = r.depth; e.face = r.face; e.used = true;
    e.bloom.assign((bits_ + 63) / 64, 0);
    const auto g = fib_.guardComponents(sv);
    e.leaf = g.empty();
    for (const auto& c : g) {
      const uint64_t h1 = compHash(c), h2 = mix64(h1 ^ 0x5bd1e995ULL) | 1;
      for (unsigned i = 0; i < k_; ++i) { const uint64_t b = (h1 + i * h2) % bits_; e.bloom[b >> 6] |= 1ULL << (b & 63); }
    }
    idx_.insert(e.key, s);
    if (++depthCount_[e.depth] == 1) depthMask_ |= 1ULL << e.depth;
    lru_.pushFront(s);
  }
  void evict(uint32_t s) {
    Entry& e = slots_[s];
    lru_.unlink(s);
    idx_.erase(e.key, [&](uint32_t v) { return v == s; });
    if (--depthCount_[e.depth] == 0) depthMask_ &= ~(1ULL << e.depth);
    e.used = false;
  }
  const G& fib_;
  const Fallback& fb_;
  size_t cap_, bits_; unsigned k_;
  uint64_t seenRevision_;
  std::vector<Entry> slots_;
  LruList lru_;
  FlatIndex idx_;
  int depthCount_[kMaxComps + 1] = {};
  uint64_t depthMask_ = 0;
  size_t size_ = 0;
  uint64_t hits_ = 0, slow_ = 0, filterPositives_ = 0;
};

// PlusBitmap Caching (Zhang et al., IFIP Networking 2020): single-hash bitmap
// over immediate children (64..4096 bits), LRU, and on a positive bitmap test
// a search restricted to depths below the cached node.
template <class Fallback, class G = TrieFib3>
class Pbc3 {
  struct Entry {
    std::string prefix; uint64_t key = 0; int depth = 0; int face = -1;
    std::vector<uint64_t> bitmap; size_t bits = 0; bool leaf = true;
  };
 public:
  Pbc3(const G& fib, const Fallback& fb, size_t capacity)
      : fib_(fib), fb_(fb), cap_(capacity), seenRevision_(fib.revision()) {
    slots_.resize(cap_); lru_.resize(cap_); idx_.init(cap_ * 2 + 1);
  }
  LpmResult lookup(std::string_view name) {
    sync();
    NameView q;
    if (!parseName<true>(name, q) || q.m == 0) return {};
    uint64_t bits = depthMask_ & maskUpTo(q.m);
    while (bits) {
      const int d = topDepth(bits);
      bits &= ~(1ULL << d);
      const auto sv = q.prefix(d);
      const uint32_t s = idx_.find(q.ph[d], [&](uint32_t v) { return bytesEqual(slots_[v].prefix, sv); });
      if (s == FlatIndex::kEmpty) continue;
      Entry& e = slots_[s];
      if (d == q.m || e.leaf || !maybe(e, q.comp(d + 1))) {
        lru_.moveFront(s); ++hits_;
        return {e.face, static_cast<int16_t>(d), true};
      }
      ++bitmapPositives_; ++slow_;
      const LpmResult base{e.face, static_cast<int16_t>(d), true};
      const LpmResult deep = fb_.resolveAbove(q, d);
      if (deep.matched) { admit(q, deep); return deep; }
      ++falseMisses_;
      return base;
    }
    ++slow_;
    const LpmResult r = fb_.resolve(q);
    admit(q, r);
    return r;
  }
  uint64_t hits() const { return hits_; }
  uint64_t slow() const { return slow_; }
  uint64_t bitmapPositives() const { return bitmapPositives_; }
  uint64_t falseMisses() const { return falseMisses_; }
  size_t cacheSize() const { return size_; }

 private:
  static bool maybe(const Entry& e, std::string_view c) {
    const uint64_t b = compHash(c) % e.bits;
    return (e.bitmap[b >> 6] & (1ULL << (b & 63))) != 0;
  }
  void sync() {
    if (seenRevision_ == fib_.revision()) return;
    for (auto& e : slots_) e = Entry{};
    lru_.resize(cap_); idx_.clear(); size_ = 0; depthMask_ = 0;
    for (int& c : depthCount_) c = 0;
    seenRevision_ = fib_.revision();
  }
  void admit(const NameView& q, const LpmResult& r) {
    if (!r.matched || cap_ == 0) return;
    const auto sv = q.prefix(r.depth);
    const uint32_t ex = idx_.find(q.ph[r.depth], [&](uint32_t v) { return bytesEqual(slots_[v].prefix, sv); });
    if (ex != FlatIndex::kEmpty) { lru_.moveFront(ex); return; }
    uint32_t s;
    if (size_ < cap_) s = static_cast<uint32_t>(size_++);
    else { s = lru_.tail(); evict(s); }
    Entry& e = slots_[s];
    e.prefix.assign(sv); e.key = q.ph[r.depth]; e.depth = r.depth; e.face = r.face;
    const auto ch = fib_.childComponents(sv);
    e.leaf = ch.empty();
    e.bits = 0; e.bitmap.clear();
    if (!ch.empty()) {
      e.bits = 64; size_t upper = 8;
      while (ch.size() > upper && e.bits < 4096) { e.bits *= 2; upper *= 2; }
      e.bitmap.assign(e.bits / 64, 0);
      for (const auto& c : ch) { const uint64_t b = compHash(c) % e.bits; e.bitmap[b >> 6] |= 1ULL << (b & 63); }
    }
    idx_.insert(e.key, s);
    if (++depthCount_[e.depth] == 1) depthMask_ |= 1ULL << e.depth;
    lru_.pushFront(s);
  }
  void evict(uint32_t s) {
    Entry& e = slots_[s];
    lru_.unlink(s);
    idx_.erase(e.key, [&](uint32_t v) { return v == s; });
    if (--depthCount_[e.depth] == 0) depthMask_ &= ~(1ULL << e.depth);
  }
  const G& fib_;
  const Fallback& fb_;
  size_t cap_;
  uint64_t seenRevision_;
  std::vector<Entry> slots_;
  LruList lru_;
  FlatIndex idx_;
  int depthCount_[kMaxComps + 1] = {};
  uint64_t depthMask_ = 0;
  size_t size_ = 0;
  uint64_t hits_ = 0, slow_ = 0, bitmapPositives_ = 0, falseMisses_ = 0;
};

// Exact full-name cache (LRU): reuses a result only when the whole name
// repeats. Included to quantify prefix locality versus name locality.
template <class Fallback, class G = TrieFib3>
class FullNameCache3 {
  struct Entry { std::string name; uint64_t key = 0; LpmResult r; };
 public:
  FullNameCache3(const G& fib, const Fallback& fb, size_t capacity)
      : fib_(fib), fb_(fb), cap_(capacity), seenRevision_(fib.revision()) {
    slots_.resize(cap_); lru_.resize(cap_); idx_.init(cap_ * 2 + 1);
  }
  LpmResult lookup(std::string_view name) {
    if (seenRevision_ != fib_.revision()) {
      lru_.resize(cap_); idx_.clear(); size_ = 0; seenRevision_ = fib_.revision();
    }
    NameView q;
    if (!parseName<true>(name, q) || q.m == 0) return {};
    const uint64_t key = q.ph[q.m];
    const uint32_t s = idx_.find(key, [&](uint32_t v) { return bytesEqual(slots_[v].name, name); });
    if (s != FlatIndex::kEmpty) { lru_.moveFront(s); ++hits_; return slots_[s].r; }
    ++slow_;
    const LpmResult r = fb_.resolve(q);
    if (cap_ == 0) return r;
    uint32_t t;
    if (size_ < cap_) t = static_cast<uint32_t>(size_++);
    else {
      t = lru_.tail(); lru_.unlink(t);
      idx_.erase(slots_[t].key, [&](uint32_t v) { return v == t; });
    }
    slots_[t].name.assign(name); slots_[t].key = key; slots_[t].r = r;
    idx_.insert(key, t); lru_.pushFront(t);
    return r;
  }
  uint64_t hits() const { return hits_; }
  uint64_t slow() const { return slow_; }
  size_t cacheSize() const { return size_; }
 private:
  const G& fib_;
  const Fallback& fb_;
  size_t cap_;
  uint64_t seenRevision_;
  std::vector<Entry> slots_;
  LruList lru_;
  FlatIndex idx_;
  size_t size_ = 0;
  uint64_t hits_ = 0, slow_ = 0;
};

}  // namespace fibv3
