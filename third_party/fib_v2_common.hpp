#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fibv2 {

struct Result {
  bool matched = false;
  std::string prefix;
  int face = -1;
  int depth = 0;
};

inline bool sameResult(const Result& a, const Result& b) {
  return a.matched == b.matched && (!a.matched ||
         (a.prefix == b.prefix && a.face == b.face && a.depth == b.depth));
}

inline std::vector<std::string> split(const std::string& name) {
  std::vector<std::string> out;
  std::string part;
  for (char c : name) {
    if (c == '/') {
      if (!part.empty()) { out.push_back(std::move(part)); part.clear(); }
    } else {
      part.push_back(c);
    }
  }
  if (!part.empty()) out.push_back(std::move(part));
  return out;
}

inline std::string join(const std::vector<std::string>& c, int n) {
  std::string out;
  for (int i = 0; i < n; ++i) { out.push_back('/'); out += c[static_cast<size_t>(i)]; }
  return out;
}

inline bool isPrefix(const std::vector<std::string>& p,
                     const std::vector<std::string>& q) {
  if (p.size() > q.size()) return false;
  for (size_t i = 0; i < p.size(); ++i) if (p[i] != q[i]) return false;
  return true;
}

class TrieFib {
  struct Node {
    std::unordered_map<std::string, std::unique_ptr<Node>> child;
    std::optional<int> face;
  };
  std::unique_ptr<Node> root_ = std::make_unique<Node>();
  uint64_t revision_ = 0;
  size_t entries_ = 0;

  const Node* walk(const std::vector<std::string>& c, int n) const {
    const Node* cur = root_.get();
    for (int i = 0; i < n; ++i) {
      auto it = cur->child.find(c[static_cast<size_t>(i)]);
      if (it == cur->child.end()) return nullptr;
      cur = it->second.get();
    }
    return cur;
  }

  static bool hasRegistered(const Node* n) {
    if (n->face) return true;
    for (const auto& kv : n->child) if (hasRegistered(kv.second.get())) return true;
    return false;
  }

 public:
  bool insert(const std::string& name, int face) {
    auto c = split(name);
    if (c.empty()) return false;
    Node* cur = root_.get();
    for (const auto& x : c) {
      auto& ptr = cur->child[x];
      if (!ptr) ptr = std::make_unique<Node>();
      cur = ptr.get();
    }
    if (!cur->face) ++entries_;
    cur->face = face;
    ++revision_;
    return true;
  }

  bool remove(const std::string& name) {
    auto c = split(name);
    if (c.empty()) return false;
    Node* cur = root_.get();
    for (const auto& x : c) {
      auto it = cur->child.find(x);
      if (it == cur->child.end()) return false;
      cur = it->second.get();
    }
    if (!cur->face) return false;
    cur->face.reset();
    --entries_;
    ++revision_;
    return true;
  }

  Result lookup(const std::string& name) const { return lookup(split(name)); }

  Result lookup(const std::vector<std::string>& c) const {
    Result best;
    const Node* cur = root_.get();
    for (size_t i = 0; i < c.size(); ++i) {
      auto it = cur->child.find(c[i]);
      if (it == cur->child.end()) break;
      cur = it->second.get();
      if (cur->face) best = {true, join(c, static_cast<int>(i + 1)),
                             *cur->face, static_cast<int>(i + 1)};
    }
    return best;
  }

  Result lookupFrom(const std::vector<std::string>& c, int startDepth,
                    Result best) const {
    const Node* cur = walk(c, startDepth);
    if (!cur) return lookup(c);
    for (size_t i = static_cast<size_t>(startDepth); i < c.size(); ++i) {
      auto it = cur->child.find(c[i]);
      if (it == cur->child.end()) break;
      cur = it->second.get();
      if (cur->face) best = {true, join(c, static_cast<int>(i + 1)),
                             *cur->face, static_cast<int>(i + 1)};
    }
    return best;
  }

  std::vector<std::string> immediateChildComponents(
      const std::string& prefix) const {
    auto c = split(prefix);
    const Node* n = walk(c, static_cast<int>(c.size()));
    std::vector<std::string> out;
    if (!n) return out;
    out.reserve(n->child.size());
    for (const auto& kv : n->child) out.push_back(kv.first);
    return out;
  }

  std::unordered_set<std::string> descendantFirstComponents(
      const std::string& prefix) const {
    auto c = split(prefix);
    const Node* n = walk(c, static_cast<int>(c.size()));
    std::unordered_set<std::string> out;
    if (!n) return out;
    for (const auto& kv : n->child)
      if (hasRegistered(kv.second.get())) out.insert(kv.first);
    return out;
  }

  uint64_t revision() const { return revision_; }
  size_t size() const { return entries_; }
};

class TrieEngine {
  const TrieFib& fib_;
 public:
  explicit TrieEngine(const TrieFib& f) : fib_(f) {}
  Result lookup(const std::string& n) { return fib_.lookup(n); }
  uint64_t fastHits() const { return 0; }
  uint64_t slowLookups() const { return 0; }
  void resetStats() {}
};

// Correct hash-table baseline. Unlike the earlier binary search, it never
// infers that a longer prefix is absent from a miss at a sparse depth.
class HashFibEngine {
  std::vector<std::unordered_map<std::string, int>> table_;
  std::vector<int> activeDepthsDesc_;
 public:
  void insert(const std::string& name, int face) {
    auto c = split(name);
    if (c.empty()) return;
    int d = static_cast<int>(c.size());
    if (table_.size() <= static_cast<size_t>(d)) table_.resize(d + 1);
    bool wasEmpty = table_[d].empty();
    table_[d][join(c, d)] = face;
    if (wasEmpty) {
      activeDepthsDesc_.push_back(d);
      std::sort(activeDepthsDesc_.begin(), activeDepthsDesc_.end(), std::greater<int>());
    }
  }
  bool remove(const std::string& name) {
    auto c = split(name); int d = static_cast<int>(c.size());
    if (d == 0 || table_.size() <= static_cast<size_t>(d)) return false;
    if (!table_[d].erase(join(c, d))) return false;
    if (table_[d].empty()) activeDepthsDesc_.erase(
      std::remove(activeDepthsDesc_.begin(), activeDepthsDesc_.end(), d), activeDepthsDesc_.end());
    return true;
  }
  Result lookup(const std::vector<std::string>& c) const {
    for (int d : activeDepthsDesc_) {
      if (d > static_cast<int>(c.size())) continue;
      std::string key = join(c, d);
      auto it = table_[d].find(key);
      if (it != table_[d].end()) return {true, std::move(key), it->second, d};
    }
    return {};
  }
  Result lookup(const std::string& name) const { return lookup(split(name)); }
  uint64_t fastHits() const { return 0; }
  uint64_t slowLookups() const { return 0; }
  void resetStats() {}
};

class Bloom {
  std::vector<uint64_t> words_;
  unsigned k_ = 3;
  size_t bits_ = 0;
  static uint64_t mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL; x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL; return x ^ (x >> 31);
  }
 public:
  Bloom(size_t bits = 128, unsigned k = 3) : words_((bits + 63) / 64), k_(k), bits_(bits) {}
  void add(const std::string& s) {
    uint64_t h = std::hash<std::string>{}(s);
    for (unsigned i = 0; i < k_; ++i) { size_t b = mix(h + i) % bits_; words_[b/64] |= 1ULL << (b%64); }
  }
  bool maybe(const std::string& s) const {
    uint64_t h = std::hash<std::string>{}(s);
    for (unsigned i = 0; i < k_; ++i) { size_t b = mix(h + i) % bits_; if (!(words_[b/64] & (1ULL << (b%64)))) return false; }
    return true;
  }
};

// Kim-style route-prefix cache. Cache decisions are made before the fallback
// trie lookup; the oracle is not precomputed, fixing the old benchmark bias.
class KimCacheEngine {
  struct Entry { std::string prefix; std::vector<std::string> comps; int face; Bloom bf; bool leaf; };
  const TrieFib& fib_; size_t capacity_; uint64_t seenRevision_;
  std::list<Entry> lru_;
  std::unordered_map<std::string, std::list<Entry>::iterator> index_;
  std::unordered_map<int, size_t> depthUse_;
  uint64_t hits_ = 0, slow_ = 0;
  void clearCache() { lru_.clear(); index_.clear(); depthUse_.clear(); }
  void sync() { if (seenRevision_ != fib_.revision()) { clearCache(); seenRevision_ = fib_.revision(); } }
  void evictBack() {
    if (lru_.empty()) return;
    auto it = std::prev(lru_.end());
    int d = static_cast<int>(it->comps.size());
    index_.erase(it->prefix);
    if (--depthUse_[d] == 0) depthUse_.erase(d);
    lru_.erase(it);
  }
  void admit(const Result& r) {
    if (!r.matched || capacity_ == 0) return;
    auto old = index_.find(r.prefix);
    if (old != index_.end()) { lru_.splice(lru_.begin(),lru_,old->second); old->second=lru_.begin(); return; }
    auto branches=fib_.descendantFirstComponents(r.prefix); Bloom b;
    for (const auto& x:branches) b.add(x);
    lru_.push_front({r.prefix,split(r.prefix),r.face,std::move(b),branches.empty()});
    index_[r.prefix]=lru_.begin(); ++depthUse_[r.depth];
    if (lru_.size()>capacity_) evictBack();
  }
 public:
  KimCacheEngine(const TrieFib& f,size_t c=1024):fib_(f),capacity_(c),seenRevision_(f.revision()){}
  Result lookup(const std::string& name) {
    sync(); auto q=split(name);
    auto best=lru_.end();
    std::vector<int> depths; depths.reserve(depthUse_.size());
    for (const auto& d : depthUse_) if (d.first <= static_cast<int>(q.size())) depths.push_back(d.first);
    std::sort(depths.begin(), depths.end(), std::greater<int>());
    for (int d : depths) { auto found=index_.find(join(q,d)); if(found!=index_.end()){best=found->second;break;} }
    if(best!=lru_.end()) {
      bool exact=best->comps.size()==q.size();
      bool safe=exact||best->leaf||!best->bf.maybe(q[best->comps.size()]);
      if(safe){ Result r{true,best->prefix,best->face,static_cast<int>(best->comps.size())}; lru_.splice(lru_.begin(),lru_,best); ++hits_; return r; }
    }
    ++slow_; Result r=fib_.lookup(q); admit(r); return r;
  }
  uint64_t fastHits()const{return hits_;} uint64_t slowLookups()const{return slow_;}
  void resetStats(){hits_=slow_=0;}
};

// PlusBitmap Caching (PBC), following Zhang et al., IFIP Networking 2020.
// A single-hash adaptive bitmap summarizes every immediate pbTrie child.
class PbcBitmap {
  std::vector<uint64_t> words_;
  size_t bits_ = 0;
 public:
  explicit PbcBitmap(size_t childCount = 0) {
    if (childCount == 0) return;
    bits_ = 64;
    size_t upper = 8;
    while (childCount > upper && bits_ < 4096) { bits_ *= 2; upper *= 2; }
    words_.assign((bits_ + 63) / 64, 0);
  }
  void add(const std::string& component) {
    if (!bits_) return;
    size_t bit = std::hash<std::string>{}(component) % bits_;
    words_[bit / 64] |= 1ULL << (bit % 64);
  }
  bool maybe(const std::string& component) const {
    if (!bits_) return false;
    size_t bit = std::hash<std::string>{}(component) % bits_;
    return (words_[bit / 64] & (1ULL << (bit % 64))) != 0;
  }
  size_t bits() const { return bits_; }
};

class PbcEngine {
  struct Entry {
    Result result;
    std::vector<std::string> comps;
    PbcBitmap bitmap;
    bool leaf = false;
  };
  const TrieFib& fib_;
  size_t capacity_;
  uint64_t seenRevision_;
  std::list<Entry> lru_;
  std::unordered_map<std::string, std::list<Entry>::iterator> index_;
  std::unordered_map<int, size_t> depthUse_;
  uint64_t hits_ = 0, slow_ = 0, bitmapPositives_ = 0, falseMisses_ = 0;

  void clearCache() { lru_.clear(); index_.clear(); depthUse_.clear(); }
  void sync() {
    if (seenRevision_ != fib_.revision()) {
      clearCache();
      seenRevision_ = fib_.revision();
    }
  }
  void touch(std::list<Entry>::iterator it) {
    lru_.splice(lru_.begin(), lru_, it);
    index_[lru_.begin()->result.prefix] = lru_.begin();
  }
  void evictBack() {
    if (lru_.empty()) return;
    auto it = std::prev(lru_.end());
    int d = it->result.depth;
    index_.erase(it->result.prefix);
    if (--depthUse_[d] == 0) depthUse_.erase(d);
    lru_.erase(it);
  }
  void admit(const Result& r) {
    if (!r.matched || capacity_ == 0) return;
    auto existing = index_.find(r.prefix);
    if (existing != index_.end()) { touch(existing->second); return; }
    auto children = fib_.immediateChildComponents(r.prefix);
    PbcBitmap bitmap(children.size());
    for (const auto& child : children) bitmap.add(child);
    lru_.push_front({r, split(r.prefix), std::move(bitmap), children.empty()});
    index_[r.prefix] = lru_.begin();
    ++depthUse_[r.depth];
    if (lru_.size() > capacity_) evictBack();
  }

 public:
  explicit PbcEngine(const TrieFib& fib, size_t capacity = 512)
      : fib_(fib), capacity_(capacity), seenRevision_(fib.revision()) {}

  Result lookup(const std::string& name) {
    sync();
    auto q = split(name);
    std::vector<int> depths;
    depths.reserve(depthUse_.size());
    for (const auto& d : depthUse_)
      if (d.first <= static_cast<int>(q.size())) depths.push_back(d.first);
    std::sort(depths.begin(), depths.end(), std::greater<int>());

    for (int d : depths) {
      auto found = index_.find(join(q, d));
      if (found == index_.end()) continue;
      auto entry = found->second;
      if (d == static_cast<int>(q.size()) || entry->leaf) {
        Result r = entry->result;
        touch(entry);
        ++hits_;
        return r;
      }
      if (!entry->bitmap.maybe(q[static_cast<size_t>(d)])) {
        Result r = entry->result;
        touch(entry);
        ++hits_;
        return r;
      }

      // Bitmap positive: paper's mid-way pbTrie search from the cached node.
      ++bitmapPositives_;
      ++slow_;
      Result base = entry->result;
      Result r = fib_.lookupFrom(q, d, base);
      if (r.depth > d) admit(r);
      else ++falseMisses_;
      return r;
    }

    ++slow_;
    Result r = fib_.lookup(q);
    admit(r);
    return r;
  }

  uint64_t fastHits() const { return hits_; }
  uint64_t slowLookups() const { return slow_; }
  uint64_t bitmapPositives() const { return bitmapPositives_; }
  uint64_t falseMisses() const { return falseMisses_; }
  void resetStats() { hits_ = slow_ = bitmapPositives_ = falseMisses_ = 0; }
};

// Layer 2, variant B1: a strictly bounded, EXACT candidate-popularity table.
// Tracks recent popularity for prefixes that are NOT currently resident in a
// HotPrefixEngine cache. Unlike the unbounded std::unordered_map<string,Score>
// it replaces, CandidateTable has a hard capacity K = k * cacheCapacity and
// never grows past it, using its own independent CLOCK second-chance sweep
// (bounded to a small probe window, mirroring the resident cache's O(1)
// admission-path cost model) to pick a victim when full.
//
// This is an intentional approximation: on eviction the candidate's
// accumulated value/tick/retryAt are simply discarded (as opposed to B2's
// frequency-sketch approximation, which keeps a lossy summary of everything
// ever seen). B1 exists primarily as an internal, easy-to-reason-about
// baseline for evaluating "does bounding memory alone hurt admission
// quality" independent of sketch estimation error.
class CandidateTable {
 public:
  struct Score {
    double value = 0.0;
    double retryAt = 0.0;
    uint64_t tick = 0;
  };

  explicit CandidateTable(size_t capacity) : capacity_(capacity) {
    slots_.reserve(capacity_);
  }

  size_t capacity() const { return capacity_; }
  size_t size() const { return index_.size(); }
  uint64_t evictions() const { return evictions_; }

  // Structural reset (FIB revision change): drop all candidates and CLOCK
  // state. Mirrors HotPrefixEngine::sync()'s treatment of its own state.
  void clear() {
    slots_.clear();
    index_.clear();
    clockHand_ = 0;
  }

  // Lifetime-counter reset, mirrors HotPrefixEngine::resetStats(); does NOT
  // touch stored candidates, only the evictions_ statistic.
  void resetStats() { evictions_ = 0; }

  // Pointer to an existing candidate's score (marking it referenced), or
  // nullptr if this prefix has no candidate record right now.
  Score* find(const std::string& prefix) {
    if (capacity_ == 0) return nullptr;
    auto it = index_.find(prefix);
    if (it == index_.end()) return nullptr;
    Slot& s = slots_[it->second];
    s.referenced = true;
    return &s.score;
  }

  // Const, side-effect-free lookup: does NOT set the referenced bit. Added
  // for BoundedExactPolicy's estimate(), which the CandidatePolicy interface
  // requires to be const with no side effects -- find() cannot satisfy that.
  const Score* peek(const std::string& prefix) const {
    if (capacity_ == 0) return nullptr;
    auto it = index_.find(prefix);
    if (it == index_.end()) return nullptr;
    return &slots_[it->second].score;
  }

  // Returns the candidate's score, creating a fresh zero-valued record if
  // absent, and marks it referenced. If capacity_ == 0 the table is
  // disabled: a scratch Score is handed back but never persisted, so no
  // history accumulates (mirrors a disabled resident cache: harmless, just
  // never remembers anything across calls).
  Score& touch(const std::string& prefix) {
    if (capacity_ == 0) {
      thread_local Score scratch;
      scratch = Score{};
      return scratch;
    }
    auto it = index_.find(prefix);
    if (it != index_.end()) {
      Slot& s = slots_[it->second];
      s.referenced = true;
      return s.score;
    }
    const size_t slot = allocateSlot();
    Slot& s = slots_[slot];
    s.prefix = prefix;
    s.score = Score{};
    s.occupied = true;
    s.referenced = true;
    index_[prefix] = slot;
    return s.score;
  }

  // Inserts or overwrites a candidate's score outright. Used when a resident
  // entry is evicted and its checkpointed popularity is handed down to
  // Layer 2, and when an admission attempt is rejected (to record the
  // rejected candidate's value/retryAt for next time).
  void insertOrUpdate(const std::string& prefix, double value, uint64_t tick,
                      double retryAt = 0.0) {
    if (capacity_ == 0) return;
    auto it = index_.find(prefix);
    if (it != index_.end()) {
      Slot& s = slots_[it->second];
      s.score.value = value;
      s.score.tick = tick;
      s.score.retryAt = retryAt;
      s.referenced = true;
      return;
    }
    const size_t slot = allocateSlot();
    Slot& s = slots_[slot];
    s.prefix = prefix;
    s.score.value = value;
    s.score.tick = tick;
    s.score.retryAt = retryAt;
    s.occupied = true;
    s.referenced = true;
    index_[prefix] = slot;
  }

  // Removes a candidate record outright. Used on promotion to resident, so
  // Layer 2 never holds a duplicate of a prefix that Layer 1 now owns.
  void erase(const std::string& prefix) {
    if (capacity_ == 0) return;
    auto it = index_.find(prefix);
    if (it == index_.end()) return;
    Slot& s = slots_[it->second];
    s.occupied = false;
    s.prefix.clear();
    index_.erase(it);
  }

 private:
  struct Slot {
    std::string prefix;
    Score score;
    bool referenced = false;
    bool occupied = false;
  };

  static constexpr size_t kProbeLimit = 8;

  // Returns a slot index ready to receive a new candidate. Grows the slot
  // vector while under capacity; once full, runs a bounded CLOCK sweep
  // (second-chance) to find a victim, evicting it (discarding its
  // value/tick/retryAt, per B1's design) if occupied.
  size_t allocateSlot() {
    if (slots_.size() < capacity_) {
      slots_.push_back(Slot{});
      return slots_.size() - 1;
    }
    size_t victim = clockHand_;
    const size_t limit = std::min(kProbeLimit, capacity_);
    for (size_t i = 0; i < limit; ++i) {
      const size_t slot = clockHand_;
      clockHand_ = (clockHand_ + 1) % capacity_;
      Slot& s = slots_[slot];
      if (!s.occupied || !s.referenced) { victim = slot; break; }
      s.referenced = false;
      victim = slot;
    }
    Slot& v = slots_[victim];
    if (v.occupied) {
      index_.erase(v.prefix);
      v.occupied = false;
      v.prefix.clear();
      ++evictions_;
    }
    return victim;
  }

  size_t capacity_ = 0;
  std::vector<Slot> slots_;
  std::unordered_map<std::string, size_t> index_;
  size_t clockHand_ = 0;
  uint64_t evictions_ = 0;
};

// Overflow-checked K = k * capacity, shared by every candidate policy that
// wants a fixed-size hint derived from the resident-cache capacity. Throws
// rather than saturating to SIZE_MAX: a saturated value would later reach a
// slots_.reserve(...)-style call in some policy and abort the process with
// std::length_error/bad_alloc anyway, so failing fast with a clear,
// catchable exception at construction time is strictly safer and more
// debuggable.
inline size_t candidateCapacityFor(size_t capacity, size_t k) {
  if (capacity == 0 || k == 0) return 0;
  if (k > std::numeric_limits<size_t>::max() / capacity) {
    throw std::overflow_error("candidate-table capacity overflow: k * capacity exceeds SIZE_MAX");
  }
  return capacity * k;
}

// ---------------------------------------------------------------------------
// CandidatePolicy: the semantic interface every Layer-2 policy satisfies.
// This is a duck-typed compile-time contract (no virtual base, no runtime
// dispatch) -- HotPrefixEngine<Policy> is instantiated per policy type. A
// conforming policy provides:
//
//   Policy(size_t capacityHint, double alpha)
//     Uniform two-argument constructor so HotPrefixEngine can build any
//     policy the same way. capacityHint's exact meaning is policy-specific
//     (an exact bound for B1, a sizing hint for B2's sketch dimensions,
//     unused for B0). alpha is a hint about how aggressively to age/decay
//     (B0/B1 use it for literal exponential decay; B2 derives an aging
//     window from it). capacityHint == 0 disables the policy for ALL three
//     implementations (B0, B1, B2): observe()/estimate() then always
//     behave as "no history persists" rather than crashing or silently
//     allocating a nonzero-capacity structure.
//
//   double estimate(const std::string& prefix, uint64_t now) const
//     Current best-effort popularity estimate for a NON-resident prefix,
//     with NO side effects. Returns 0.0 if the prefix has no record. `now`
//     is HotPrefixEngine's own tick_ (advances once per lookup(), on both
//     hits and misses) -- passed explicitly so B0/B1's decay is driven by
//     total elapsed lookup time, not by how many non-resident observations
//     happened to occur. B2 may ignore `now`, since its aging is
//     deliberately observation-count-based rather than time-based.
//
//   double observe(const std::string& prefix, uint64_t now)
//     Called once per slow-path hit for a matched, non-resident prefix.
//     Records the observation (decayed against `now`, where applicable)
//     and returns the updated estimate.
//
//   void checkpoint(const std::string& prefix, double value, uint64_t now)
//     Called when a resident entry is evicted, handing its checkpointed
//     popularity down to Layer 2 so a later re-admission decision is not
//     penalized back to zero. Exact policies (B0/B1) store this precisely;
//     the sketch (B2) folds it in approximately by raising its estimate
//     TOWARD `value` (not by blindly adding `value` increments -- see
//     BoundedFrequencySketch::checkpoint for why that matters).
//
//   bool eligibleForAdmission(const std::string& prefix, double estimate) const
//     Side-effect-free pre-check, called BEFORE any resident CLOCK state is
//     touched (i.e. before chooseVictim() would run). Lets B0/B1 enforce a
//     "recently rejected, don't retry yet" cooldown without the cost of a
//     spurious chooseVictim() sweep (chooseVictim() clears referenced bits
//     as a side effect, so calling it even for doomed candidates would
//     perturb resident eviction timing). B2 has no per-candidate cooldown
//     state, so it always returns true here.
//
//   bool beatsVictim(const std::string& prefix, double estimate,
//                    double victimScore, uint64_t now)
//     Called only once a real victim's score is known (i.e. only when
//     eligibleForAdmission() already returned true AND every resident slot
//     is full). Decides whether `estimate` convincingly beats `victimScore`.
//     May have side effects (B0/B1 record a rejection cooldown; B2 does
//     not).
//
//   void onPromotion(const std::string& prefix)
//     Called when `prefix` is actually promoted to resident. Layer 2 should
//     drop or otherwise stop actively tracking this prefix afterward, since
//     Layer 1 (the resident Entry) now owns its popularity. B0/B1 erase it
//     exactly; B2 uses an approximate conservative retirement because its
//     counters are shared. This trades a small collision-induced
//     under-estimation risk for avoiding residual-mass re-admission loops.
//
//   void clear()
//     Full reset, called on FIB revision change.
//
//   void resetStats()
//     Reset lifetime counters only (not stored candidate state).
//
// Correctness note: NONE of these methods influence the face or prefix
// returned by HotPrefixEngine::lookup() for any query -- they only
// influence which prefixes become resident and when. LPM correctness is
// therefore independent of the policy's estimation error by construction
// (see the admission-policy independence theorem discussed alongside this
// design). A buggy or adversarial policy can only hurt performance/hit
// ratio, never return a wrong forwarding decision, because every resident
// entry's `guardedBranches` safety check and the exact fallback resolver
// are completely unaffected by which policy is plugged in.
// ---------------------------------------------------------------------------

// B0: UnboundedCandidatePolicy -- reproduces the ORIGINAL, unbounded
// per-prefix exact tracking (one Score per distinct prefix ever observed on
// the slow path, never evicted except on clear()). This is intentionally
// the "ground truth" admission-accuracy oracle: it has no capacity limit and
// no estimation error, so B1/B2's memory-vs-accuracy trade-offs are always
// measured against it, not against each other.
class UnboundedCandidatePolicy {
  struct Entry {
    double value = 0.0;
    double retryAt = 0.0;
    uint64_t tick = 0;
  };

  double currentValue(const Entry& e, uint64_t now) const {
    if (alpha_ == 1.0 || e.tick == now) return e.value;
    const uint64_t elapsed = now - e.tick;
    if (elapsed == 1) return e.value * alpha_;
    return e.value * std::pow(alpha_, static_cast<double>(elapsed));
  }

 public:
  // capacityHint is otherwise ignored (B0 is unbounded by design), except
  // that 0 disables the policy entirely, per the shared
  // "capacityHint == 0 -> disabled" convention across all three policies.
  UnboundedCandidatePolicy(size_t capacityHint, double alpha)
      : alpha_(alpha), disabled_(capacityHint == 0) {}

  double estimate(const std::string& prefix, uint64_t now) const {
    if (disabled_) return 0.0;
    auto it = table_.find(prefix);
    return it == table_.end() ? 0.0 : currentValue(it->second, now);
  }

  double observe(const std::string& prefix, uint64_t now) {
    if (disabled_) return 0.0;
    Entry& e = table_[prefix];
    e.value = currentValue(e, now) + 1.0;
    e.tick = now;
    return e.value;
  }

  void checkpoint(const std::string& prefix, double value, uint64_t now) {
    if (disabled_) return;
    Entry& e = table_[prefix];
    e.value = value;
    e.tick = now;
    e.retryAt = 0.0;
  }

  // Side-effect-free: safe to call before any resident CLOCK state (i.e.
  // before chooseVictim()) is touched.
  bool eligibleForAdmission(const std::string& prefix, double est) const {
    if (disabled_) return true;
    auto it = table_.find(prefix);
    const double retryAt = (it != table_.end()) ? it->second.retryAt : 0.0;
    return est >= retryAt;
  }

  bool beatsVictim(const std::string& prefix, double est, double victimScore, uint64_t now) {
    if (disabled_) return est > victimScore;
    if (est <= victimScore) {
      Entry& e = table_[prefix];
      e.value = est;
      e.tick = now;
      e.retryAt = victimScore + 1.0;
      return false;
    }
    return true;
  }

  void onPromotion(const std::string& prefix) { table_.erase(prefix); }
  void clear() { table_.clear(); }
  void resetStats() {}

  // Metrics (not part of the common interface; accessed via
  // HotPrefixEngine::candidatePolicy() when the concrete type is known).
  size_t size() const { return table_.size(); }

 private:
  std::unordered_map<std::string, Entry> table_;
  double alpha_;
  bool disabled_;
};

// B1: BoundedExactPolicy -- a thin adapter exposing the CandidatePolicy
// semantic interface over the UNCHANGED CandidateTable (Stage 2's bounded,
// exact, own-CLOCK candidate table). CandidateTable itself is not modified
// by Stage 3 beyond the earlier addition of a const peek(); all of its
// admission/eviction mechanics remain exactly as validated in Stage 2.
class BoundedExactPolicy {
  double currentValue(const CandidateTable::Score& s, uint64_t now) const {
    if (alpha_ == 1.0 || s.tick == now) return s.value;
    const uint64_t elapsed = now - s.tick;
    if (elapsed == 1) return s.value * alpha_;
    return s.value * std::pow(alpha_, static_cast<double>(elapsed));
  }

 public:
  // CandidateTable already treats capacity==0 as "disabled" internally
  // (peek/find return nullptr, touch hands back an unpersisted scratch
  // value, insertOrUpdate/erase are no-ops), so BoundedExactPolicy needs no
  // extra disabled_ flag of its own -- it inherits the shared
  // "capacityHint == 0 -> disabled" convention for free.
  BoundedExactPolicy(size_t candidateCapacity, double alpha)
      : table_(candidateCapacity), alpha_(alpha) {}

  double estimate(const std::string& prefix, uint64_t now) const {
    const CandidateTable::Score* s = table_.peek(prefix);
    return s ? currentValue(*s, now) : 0.0;
  }

  double observe(const std::string& prefix, uint64_t now) {
    CandidateTable::Score& s = table_.touch(prefix);
    s.value = currentValue(s, now) + 1.0;
    s.tick = now;
    return s.value;
  }

  void checkpoint(const std::string& prefix, double value, uint64_t now) {
    table_.insertOrUpdate(prefix, value, now, 0.0);
  }

  // Side-effect-free: safe to call before any resident CLOCK state (i.e.
  // before chooseVictim()) is touched.
  bool eligibleForAdmission(const std::string& prefix, double est) const {
    const CandidateTable::Score* s = table_.peek(prefix);
    const double retryAt = s ? s->retryAt : 0.0;
    return est >= retryAt;
  }

  bool beatsVictim(const std::string& prefix, double est, double victimScore, uint64_t now) {
    if (est <= victimScore) {
      table_.insertOrUpdate(prefix, est, now, victimScore + 1.0);
      return false;
    }
    return true;
  }

  void onPromotion(const std::string& prefix) { table_.erase(prefix); }
  void clear() { table_.clear(); }
  void resetStats() { table_.resetStats(); }

  // Metrics (accessed via HotPrefixEngine::candidatePolicy()).
  size_t size() const { return table_.size(); }
  size_t capacity() const { return table_.capacity(); }
  uint64_t evictions() const { return table_.evictions(); }

 private:
  CandidateTable table_;
  double alpha_;
};

// B2: BoundedFrequencySketch -- a Count-Min Sketch with saturating counters,
// conservative updates, a Doorkeeper (first-observation gate), and periodic
// aging (halving). Memory is O(depth*width + doorkeeperBits), fixed at
// construction and INDEPENDENT of the number of distinct prefixes ever
// observed -- the key property that motivated B2 over B0/B1's per-prefix
// storage.
//
// Design decision on scope (see the wider discussion this resolves): B2
// tracks ONLY non-resident candidates, mirroring B0/B1 -- it is not a
// global sketch that also observes resident hits. This keeps the resident
// fast path completely free of sketch hashing (consistent with Layer 1
// being independent of the Layer-2 policy). On promotion, B2 can
// conservatively retire the key's estimated mass from its addressed
// counters. This prevents immediate re-admission after cooling, at the cost
// of possible under-estimation for colliding keys. The behavior is an
// explicit Config ablation rather than an undocumented side effect.
//
// Other documented approximations relative to B0/B1 (each intentional and
// measured, not an oversight):
//  - Counters here are plain uint8_t (1 byte each), NOT bit-packed to 4
//    bits. logicalBytes() reports the true allocation; a bit-packed layout
//    is left as future work, since packing correctness needs its own
//    dedicated testing, orthogonal to validating this architecture.
//  - checkpoint() cannot store an exact value in shared counters; it RAISES
//    the sketch's estimate toward `value` via bounded conservative
//    increments, stopping as soon as the estimate reaches the target
//    (rather than always adding `value` more increments on top of whatever
//    the sketch already believes, which would systematically overcount a
//    prefix that already had residual mass from a prior promotion).
//  - eligibleForAdmission()/beatsVictim() carry no per-candidate
//    cooldown/retryAt state (unlike B0/B1): a rejected candidate can be
//    reconsidered on its very next observation. The cost of that is
//    bounded (O(depth) hash+read per attempt), the trade B2 makes for
//    O(1)-memory-per-key state.
class BoundedFrequencySketch {
 public:
  // Explicit knobs for ablation studies (sketch width, counter depth,
  // Doorkeeper on/off, aging window). Not used by HotPrefixEngine's default
  // uniform (capacityHint, alpha) construction path -- constructed directly
  // and injected via HotPrefixEngine's policy-injection constructor when a
  // specific configuration needs to be compared against the defaults.
  struct Config {
    size_t depth = 4;
    size_t width = 1024;      // rounded up to the next power of two
    bool useDoorkeeper = true;
    uint64_t agingWindow = 4096;
    // Approximate deletion at promotion. Each row counter addressed by the
    // promoted key is reduced by that key's current minimum estimate. This
    // removes residual mass that otherwise causes immediate re-admission
    // after cooling. Because counters are shared, it can under-estimate
    // colliding keys; it is therefore exposed as an explicit ablation.
    bool retireOnPromotion = true;
  };

  explicit BoundedFrequencySketch(Config cfg)
      : disabled_(false), useDoorkeeper_(cfg.useDoorkeeper),
        retireOnPromotion_(cfg.retireOnPromotion) {
    width_ = nextPow2(std::max<size_t>(1, cfg.width));
    depth_ = std::max<size_t>(1, cfg.depth);
    if (width_ > std::numeric_limits<size_t>::max() / depth_) {
      throw std::overflow_error("frequency-sketch counter size overflow");
    }
    const size_t counterCount = depth_ * width_;
    counters_.assign(counterCount, 0);
    if (useDoorkeeper_) {
      const size_t doorkeeperBytes = width_ / 8 + static_cast<size_t>(width_ % 8 != 0);
      doorkeeper_.assign(doorkeeperBytes, 0);
    }
    agingWindow_ = std::max<uint64_t>(1, cfg.agingWindow);
    seeds_ = {0x9E3779B97F4A7C15ULL, 0xC2B2AE3D27D4EB4FULL,
              0x165667B19E3779F9ULL, 0x27D4EB2F165667C5ULL};
  }

  // capacityHint == 0 disables the sketch entirely (per the shared
  // "capacityHint == 0 -> disabled" convention across all three policies):
  // no counters/doorkeeper are allocated, and observe()/estimate() always
  // report "never seen".
  BoundedFrequencySketch(size_t capacityHint, double alpha)
      : disabled_(capacityHint == 0), useDoorkeeper_(true),
        retireOnPromotion_(true) {
    if (disabled_) {
      width_ = 0;
      depth_ = 0;
      return;
    }
    width_ = nextPow2(std::max<size_t>(64, capacityHint));
    depth_ = 4;
    // depth_ * width_ and the doorkeeper's byte count must both be checked
    // for overflow before allocating: nextPow2()'s clamp to the largest
    // representable power of two prevents width_ itself from wrapping, but
    // does NOT make depth_*width_ (4x larger) automatically representable.
    if (width_ > std::numeric_limits<size_t>::max() / depth_) {
      throw std::overflow_error("frequency-sketch counter size overflow");
    }
    const size_t counterCount = depth_ * width_;
    const size_t doorkeeperBytes = width_ / 8 + static_cast<size_t>(width_ % 8 != 0);
    counters_.assign(counterCount, 0);
    doorkeeper_.assign(doorkeeperBytes, 0);
    if (!(alpha > 0.0 && alpha <= 1.0))
      throw std::invalid_argument("alpha must be in (0,1]");
    if (alpha < 1.0) {
      // Match one sketch halving pass to the exact policy's exponential
      // half-life. observe() runs only for slow non-resident matches, so
      // this remains an approximation, but it is dimensionally aligned
      // with alpha instead of scaling incorrectly with table capacity.
      const double halfLife = std::log(0.5) / std::log(alpha);
      agingWindow_ = std::max<uint64_t>(64, static_cast<uint64_t>(std::llround(halfLife)));
    } else {
      // In stationary mode, use four sketch-width equivalents. This retained
      // near-oracle steady-state hit ratio while recovering from scan
      // pollution substantially better than an effectively unaged sketch.
      constexpr uint64_t stationaryMultiplier = 4;
      agingWindow_ = capacityHint > std::numeric_limits<uint64_t>::max()/stationaryMultiplier
          ? std::numeric_limits<uint64_t>::max()
          : std::max<uint64_t>(1024, static_cast<uint64_t>(capacityHint)*stationaryMultiplier);
    }
    seeds_ = {0x9E3779B97F4A7C15ULL, 0xC2B2AE3D27D4EB4FULL,
              0x165667B19E3779F9ULL, 0x27D4EB2F165667C5ULL};
  }

  // `now` is accepted for interface uniformity but ignored: B2's aging is
  // deliberately observation-count-based (maybeAge()), not time-based.
  double estimate(const std::string& prefix, uint64_t /*now*/) const {
    if (disabled_) return 0.0;
    if (!inDoorkeeper(prefix)) return 0.0;
    return 1.0 + static_cast<double>(cmsEstimate(prefix));
  }

  double observe(const std::string& prefix, uint64_t /*now*/) {
    if (disabled_) return 0.0;
    ++observations_;
    // Age BEFORE recording this observation. If this call lands exactly on
    // the aging boundary, the Doorkeeper/counters used below must already
    // reflect the post-aging state -- otherwise observe() would return a
    // pre-aging value while the very next estimate() call (e.g. from
    // admit()) sees the post-aging one, and they'd disagree.
    maybeAge();
    if (!inDoorkeeper(prefix)) {
      setDoorkeeper(prefix);
      // Do NOT assume a doorkeeper-miss means cmsEstimate==0: hash
      // collisions, residual mass left over from a promoted-then-aged-out
      // prefix, or counter aging can all leave nonzero counters even for a
      // prefix whose OWN doorkeeper bit was just cleared/never set. Read
      // the actual estimate so this stays consistent with estimate().
      return 1.0 + static_cast<double>(cmsEstimate(prefix));
    }
    cmsIncrement(prefix);
    return 1.0 + static_cast<double>(cmsEstimate(prefix));
  }

  // Raises the sketch's estimate for `prefix` TOWARD `value`, rather than
  // always adding `value` more (which would systematically overcount a
  // prefix that already carries residual mass from a previous promotion,
  // since onPromotion() cannot exactly erase it -- see class comment).
  // Guards against non-finite/negative input before any double->size_t
  // conversion.
  void checkpoint(const std::string& prefix, double value, uint64_t now) {
    if (disabled_) return;
    if (!std::isfinite(value) || value <= 0.0) return;
    if (!inDoorkeeper(prefix)) setDoorkeeper(prefix);
    const double target = std::min(value, 254.0); // stay clear of counter saturation (255)
    constexpr size_t kMaxRaiseSteps = 64;          // bounded work per checkpoint
    for (size_t steps = 0;
         steps < kMaxRaiseSteps && estimate(prefix, now) < target;
         ++steps) {
      cmsIncrement(prefix);
    }
  }

  // No per-candidate cooldown state, so always eligible; the real decision
  // happens in beatsVictim() once a victim's score is known.
  bool eligibleForAdmission(const std::string& /*prefix*/, double /*est*/) const {
    return true;
  }

  bool beatsVictim(const std::string& /*prefix*/, double est, double victimScore,
                   uint64_t /*now*/) {
    return est > victimScore;
  }

  void onPromotion(const std::string& prefix) {
    if (disabled_ || !retireOnPromotion_) return;
    const uint8_t amount = cmsEstimate(prefix);
    if (amount == 0) return;
    for (size_t r = 0; r < depth_; ++r) {
      const size_t idx = r * width_ + hashSlot(prefix, r);
      counters_[idx] = counters_[idx] > amount
          ? static_cast<uint8_t>(counters_[idx] - amount)
          : 0;
    }
    ++promotionRetirements_;
  }

  void clear() {
    std::fill(counters_.begin(), counters_.end(), 0);
    std::fill(doorkeeper_.begin(), doorkeeper_.end(), 0);
    observations_ = 0;
    agingPasses_ = 0;
    promotionRetirements_ = 0;
  }

  void resetStats() { agingPasses_ = promotionRetirements_ = 0; }

  // Metrics (accessed via HotPrefixEngine::candidatePolicy()).
  size_t depth() const { return depth_; }
  size_t width() const { return width_; }
  uint64_t agingPasses() const { return agingPasses_; }
  uint64_t agingWindow() const { return agingWindow_; }
  bool doorkeeperEnabled() const { return useDoorkeeper_; }
  bool retireOnPromotionEnabled() const { return retireOnPromotion_; }
  uint64_t promotionRetirements() const { return promotionRetirements_; }
  size_t logicalBytes() const { return counters_.size() * sizeof(uint8_t) + doorkeeper_.size(); }

 public:
  // Exposed for direct unit testing of the overflow-safe rounding logic
  // without needing to trigger a real (potentially exabyte-scale)
  // allocation through full sketch construction.
  static size_t nextPow2(size_t v) {
    if (v <= 1) return 1;
    constexpr int kBits = std::numeric_limits<size_t>::digits;
    const size_t highestPow2 = size_t(1) << (kBits - 1);
    if (v > highestPow2) return highestPow2;
    size_t p = 1;
    while (p < v) p <<= 1;
    return p;
  }

 private:

  size_t hashSlot(const std::string& prefix, size_t row) const {
    uint64_t h = std::hash<std::string>{}(prefix) ^ seeds_[row % seeds_.size()];
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33;
    return static_cast<size_t>(h % width_);
  }

  bool inDoorkeeper(const std::string& prefix) const {
    if (!useDoorkeeper_) return true; // Doorkeeper disabled: treat as always-seen
    const uint64_t h = std::hash<std::string>{}(prefix);
    const size_t bit = h % (doorkeeper_.size() * 8);
    return (doorkeeper_[bit / 8] >> (bit % 8)) & 1u;
  }

  void setDoorkeeper(const std::string& prefix) {
    if (!useDoorkeeper_) return;
    const uint64_t h = std::hash<std::string>{}(prefix);
    const size_t bit = h % (doorkeeper_.size() * 8);
    doorkeeper_[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
  }

  uint8_t cmsEstimate(const std::string& prefix) const {
    uint8_t m = 255;
    for (size_t r = 0; r < depth_; ++r)
      m = std::min(m, counters_[r * width_ + hashSlot(prefix, r)]);
    return m;
  }

  void cmsIncrement(const std::string& prefix) {
    const uint8_t m = cmsEstimate(prefix);
    if (m >= 255) return; // saturated
    for (size_t r = 0; r < depth_; ++r) {
      const size_t idx = r * width_ + hashSlot(prefix, r);
      if (counters_[idx] == m) counters_[idx] = static_cast<uint8_t>(counters_[idx] + 1);
    }
  }

  void maybeAge() {
    if (agingWindow_ == 0 || observations_ % agingWindow_ != 0) return;
    for (auto& c : counters_) c = static_cast<uint8_t>(c >> 1);
    std::fill(doorkeeper_.begin(), doorkeeper_.end(), 0);
    ++agingPasses_;
  }

  bool disabled_ = false;
  bool useDoorkeeper_ = true;
  bool retireOnPromotion_ = true;
  size_t depth_ = 4, width_ = 1024;
  std::vector<uint8_t> counters_;
  std::vector<uint8_t> doorkeeper_;
  uint64_t agingWindow_ = 1024;
  uint64_t observations_ = 0;
  uint64_t agingPasses_ = 0;
  uint64_t promotionRetirements_ = 0;
  std::array<uint64_t, 4> seeds_{};
};

// Per-lookup admission outcome used only for validation/diagnostics. It does
// not participate in lookup or replacement decisions.
enum class AdmissionOutcome : uint8_t {
  None = 0,
  BelowPromotion,
  CooldownRejected,
  VictimRejected,
  AdmittedFree,
  AdmittedReplace
};

// Hot-prefix cache optimized for the lookup path.  The old implementation
// allocated and sorted a depth vector, joined prefix strings, performed nested
// hash lookups, and updated an ordered ranking tree on every hit.  This version
// stores admitted prefixes in a compact trie and uses a CLOCK second-chance bit
// for O(1) hit maintenance.  String construction, guard discovery and eviction
// work happen only on the slow admission path.
//
// CandidatePolicy is selected at compile time (see the CandidatePolicy
// interface documentation above CandidateTable/UnboundedCandidatePolicy/
// BoundedFrequencySketch): Layer 1 (this class's resident CLOCK cache) is
// completely independent of which Layer-2 policy is plugged in, and is
// identical across all instantiations.
template <class CandidatePolicy>
class HotPrefixEngine {
  struct Entry;
  struct CacheNode {
    std::unordered_map<std::string, std::unique_ptr<CacheNode>> child;
    Entry* entry = nullptr;
  };
  // Layer 1 state: exact, resident-only popularity tracking, owned directly
  // by the Entry. No pointer into any external map, no shared ownership, no
  // dependency on Layer 2's memory bound. |entries_| <= capacity_ always
  // holds, so this layer is O(capacity_) by construction.
  struct Entry {
    Result result;
    std::vector<std::string> comps;
    std::unordered_set<std::string> guardedBranches;
    double residentScore = 0.0;
    uint64_t scoreTick = 0;
    size_t slot = 0;
    bool referenced = true;
  };

  const TrieFib& fib_;
  const HashFibEngine* fallbackHash_ = nullptr;
  uint64_t fallbackRevision_ = 0;
  size_t capacity_;
  double promote_, alpha_, cool_;
  uint64_t tick_ = 0, seenRevision_, hits_ = 0, slow_ = 0, evictions_ = 0;
#ifdef FIBV2_VALIDATION
  uint64_t promotionCrossings_ = 0, cooldownRejections_ = 0;
  uint64_t victimRejections_ = 0, admissions_ = 0;
  uint64_t replacementEvictions_ = 0, coolingEvictions_ = 0;
  AdmissionOutcome lastAdmissionOutcome_ = AdmissionOutcome::None;
#endif
  size_t candidateK_ = 4;
  CandidatePolicy policy_;
  std::unordered_map<std::string, std::unique_ptr<Entry>> entries_;
  CacheNode cacheRoot_;
  std::vector<Entry*> slots_;
  size_t clockHand_ = 0;
  static constexpr uint64_t learningEpoch_ = 65536;

  // Same exponential-decay model previously shared with Layer-2's Score
  // object, now applied only to a resident Entry's own fields (Layer 2's
  // decay/aging is entirely internal to whichever CandidatePolicy is
  // plugged in).
  double currentResident(const Entry& e) const {
    if (alpha_ == 1.0 || e.scoreTick == tick_) return e.residentScore;
    const uint64_t elapsed = tick_ - e.scoreTick;
    if (elapsed == 1) return e.residentScore * alpha_;
    return e.residentScore * std::pow(alpha_, static_cast<double>(elapsed));
  }

  double touchResident(Entry& e) {
    e.residentScore = currentResident(e) + 1.0;
    e.scoreTick = tick_;
    return e.residentScore;
  }

  void sync() {
    if (seenRevision_ == fib_.revision()) return;
    policy_.clear();
    entries_.clear();
    cacheRoot_ = CacheNode{};
    slots_.clear();
    clockHand_ = 0;
    tick_ = 0;
    // A supplied adaptive-hash resolver is a snapshot. After a dynamic FIB
    // mutation it may be stale, so correctness takes precedence and misses
    // revert to the authoritative trie.
    if (fallbackHash_ && fallbackRevision_ != fib_.revision())
      fallbackHash_ = nullptr;
    seenRevision_ = fib_.revision();
  }

  static bool removePath(CacheNode& node,
                         const std::vector<std::string>& comps,
                         size_t index) {
    if (index == comps.size()) {
      node.entry = nullptr;
    } else {
      auto it = node.child.find(comps[index]);
      if (it != node.child.end() && removePath(*it->second, comps, index + 1))
        node.child.erase(it);
    }
    return node.entry == nullptr && node.child.empty();
  }

  enum class RemovalCause { Replacement, Cooling };

  void eraseSlot(size_t slot, RemovalCause cause) {
    Entry* victim = slots_[slot];
    if (!victim) return;
    const std::string prefix = victim->result.prefix;
    removePath(cacheRoot_, victim->comps, 0);
    // Preserve accumulated popularity as a Layer-2 candidate score so a
    // later re-admission decision is not penalized back to zero. This
    // reproduces the old shared-Score behavior explicitly: the decayed
    // value is checkpointed "as of now" (tick_), which is mathematically
    // equivalent to leaving it stale, since alpha^a * alpha^b == alpha^(a+b).
    policy_.checkpoint(prefix, currentResident(*victim), tick_);
    slots_[slot] = nullptr;
    entries_.erase(prefix);
    ++evictions_;
#ifdef FIBV2_VALIDATION
    if (cause == RemovalCause::Replacement) {
      ++replacementEvictions_;
    } else {
      ++coolingEvictions_;
    }
#else
    (void)cause;
#endif
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
      Entry* entry = slots_[slot];
      if (!entry) return slot;
      const bool wasReferenced = entry->referenced;
      entry->referenced = false;
      const double score = currentResident(*entry);
      if ((!wasReferenced && (!foundUnreferenced || score < bestScore)) ||
          (wasReferenced && !foundUnreferenced && score < bestScore)) {
        best = slot;
        bestScore = score;
        foundUnreferenced = !wasReferenced;
      }
    }
    return best;
  }

  void insertPath(Entry* entry) {
    CacheNode* node = &cacheRoot_;
    for (const auto& component : entry->comps) {
      auto& next = node->child[component];
      if (!next) next = std::make_unique<CacheNode>();
      node = next.get();
    }
    node->entry = entry;
  }

  void admit(const Result& result) {
    if (capacity_ == 0 || !result.matched) return;
    // entries_ is the sole source of truth for residency now that resident
    // state no longer lives inside scores_.
    if (entries_.count(result.prefix)) return;

    double candidateValue = policy_.estimate(result.prefix, tick_);

    size_t slot;
    bool replacedResident = false;
    if (entries_.size() < capacity_) {
      auto empty = std::find(slots_.begin(), slots_.end(), nullptr);
      if (empty != slots_.end()) {
        slot = static_cast<size_t>(empty - slots_.begin());
      } else {
        slot = slots_.size();
        slots_.push_back(nullptr);
      }
    } else {
      slot = chooseVictim();
      Entry* victim = slots_[slot];
      // Do not let a merely threshold-crossing cold prefix displace a hotter
      // resident. The policy decides whether candidateValue convincingly
      // beats the victim's score (and may record its own rejection
      // bookkeeping, e.g. a cooldown, as a side effect). By this point
      // chooseVictim() has already run -- the cheaper, side-effect-free
      // eligibleForAdmission() pre-check in lookup() is what keeps a
      // cooling-down candidate from reaching this point at all.
      const double victimScore = victim ? currentResident(*victim) : 0.0;
      if (victim && !policy_.beatsVictim(result.prefix, candidateValue, victimScore, tick_)) {
#ifdef FIBV2_VALIDATION
        ++victimRejections_;
        lastAdmissionOutcome_ = AdmissionOutcome::VictimRejected;
#endif
        return;
      }
      eraseSlot(slot, RemovalCause::Replacement);
      replacedResident = true;
    }

#ifdef FIBV2_VALIDATION
    lastAdmissionOutcome_ = replacedResident ? AdmissionOutcome::AdmittedReplace
                                             : AdmissionOutcome::AdmittedFree;
#else
    (void)replacedResident;
#endif

    auto entry = std::make_unique<Entry>();
    entry->result = result;
    entry->comps = split(result.prefix);
    entry->guardedBranches = fib_.descendantFirstComponents(result.prefix);
    entry->residentScore = candidateValue;
    entry->scoreTick = tick_;
    entry->slot = slot;
    Entry* raw = entry.get();
    entries_.emplace(result.prefix, std::move(entry));
    // Promoted to resident: Layer 2 should only track prefixes that are NOT
    // currently cached, so let the policy stop tracking this one.
    policy_.onPromotion(result.prefix);
    slots_[slot] = raw;
    insertPath(raw);
#ifdef FIBV2_VALIDATION
    ++admissions_;
#endif
  }

  Entry* probe(const std::vector<std::string>& query) {
    CacheNode* node = &cacheRoot_;
    for (size_t i = 0; i < query.size(); ++i) {
      auto it = node->child.find(query[i]);
      if (it == node->child.end()) return nullptr;
      node = it->second.get();
      Entry* entry = node->entry;
      if (!entry) continue;

      const size_t depth = i + 1;
      const bool safe = depth == query.size() ||
                        entry->guardedBranches.empty() ||
                        !entry->guardedBranches.count(query[depth]);
      if (!safe) continue;

      // The non-decaying benchmark variant performs only these two stores on
      // a hit.  No map lookup, allocation, logarithm, pow, or ordered-tree
      // update remains in its fast path.
      if (alpha_ < 1.0 && cool_ > 0.0 && currentResident(*entry) < cool_) {
        const size_t slot = entry->slot;
        eraseSlot(slot, RemovalCause::Cooling);
        return nullptr;
      }
      if (alpha_ < 1.0) {
        touchResident(*entry);
      } else {
        entry->residentScore += 1.0;
      }
      entry->referenced = true;
      return entry;
    }
    return nullptr;
  }

 public:
  HotPrefixEngine(const TrieFib& fib, size_t capacity = 1024,
                  double promotion = 4, double alpha = .995,
                  double cool = .25, size_t candidateTableK = 4)
      : fib_(fib), fallbackRevision_(fib.revision()), capacity_(capacity),
        promote_(promotion), alpha_(alpha), cool_(cool),
        seenRevision_(fib.revision()), candidateK_(candidateTableK),
        policy_(candidateCapacityFor(capacity, candidateTableK), alpha) {
    if (!(alpha_ > 0.0 && alpha_ <= 1.0))
      throw std::invalid_argument("alpha must be in (0,1]");
    if (alpha_ < 1.0 && promote_ >= 1.0 / (1.0 - alpha_))
      throw std::invalid_argument("unreachable promotion threshold for alpha");
    slots_.reserve(capacity_);
    entries_.reserve(capacity_ * 2 + 1);
  }

  HotPrefixEngine(const TrieFib& fib, const HashFibEngine& fallbackHash,
                  size_t capacity = 1024, double promotion = 4,
                  double alpha = .995, double cool = .25,
                  size_t candidateTableK = 4)
      : HotPrefixEngine(fib, capacity, promotion, alpha, cool, candidateTableK) {
    fallbackHash_ = &fallbackHash;
    fallbackRevision_ = fib_.revision();
  }

  // Policy-injection constructor: accepts an already-constructed
  // CandidatePolicy instead of building one via the uniform
  // (capacityHint, alpha) path above. Used for ablation studies where a
  // policy needs explicit configuration beyond what the uniform
  // constructor exposes (e.g. BoundedFrequencySketch built from a
  // Config{depth, width, useDoorkeeper, agingWindow}).
  HotPrefixEngine(const TrieFib& fib, CandidatePolicy customPolicy,
                  size_t capacity = 1024, double promotion = 4,
                  double alpha = .995, double cool = .25)
      : fib_(fib), fallbackRevision_(fib.revision()), capacity_(capacity),
        promote_(promotion), alpha_(alpha), cool_(cool),
        seenRevision_(fib.revision()), candidateK_(0),
        policy_(std::move(customPolicy)) {
    if (!(alpha_ > 0.0 && alpha_ <= 1.0))
      throw std::invalid_argument("alpha must be in (0,1]");
    if (alpha_ < 1.0 && promote_ >= 1.0 / (1.0 - alpha_))
      throw std::invalid_argument("unreachable promotion threshold for alpha");
    slots_.reserve(capacity_);
    entries_.reserve(capacity_ * 2 + 1);
  }

  HotPrefixEngine(const TrieFib& fib, const HashFibEngine& fallbackHash,
                  CandidatePolicy customPolicy, size_t capacity = 1024,
                  double promotion = 4, double alpha = .995, double cool = .25)
      : HotPrefixEngine(fib, std::move(customPolicy), capacity, promotion, alpha, cool) {
    fallbackHash_ = &fallbackHash;
    fallbackRevision_ = fib_.revision();
  }

  Result lookup(const std::string& name) {
#ifdef FIBV2_VALIDATION
    lastAdmissionOutcome_ = AdmissionOutcome::None;
#endif
    ++tick_;
    sync();
    auto query = split(name);
    if (query.empty()) return {};

    if (Entry* entry = probe(query)) {
      ++hits_;
      return entry->result;
    }

    ++slow_;
    Result result = fallbackHash_ ? fallbackHash_->lookup(query)
                                  : fib_.lookup(query);
    if (!result.matched) return result;

    // Edge case: probe() can skip an already-resident entry when a guarded
    // branch makes it unsafe to return directly, yet the authoritative
    // resolver still lands on that same resident prefix. The original
    // implementation updated the shared Score object in this situation (it
    // was reachable via both the Entry and scores_), so popularity kept
    // accruing even though the answer came back through the slow resolver.
    // Reproduce that here explicitly, now that resident state is no longer
    // shared with scores_. This only runs on the slow path, so it adds no
    // cost to probe()'s fast path.
    {
      auto residentIt = entries_.find(result.prefix);
      if (residentIt != entries_.end()) {
        Entry& resident = *residentIt->second;
        if (alpha_ < 1.0) {
          touchResident(resident);
        } else {
          resident.residentScore += 1.0;
        }
        resident.referenced = true;
        return result;
      }
    }

    // The stationary (non-decaying) model learns a hot set during a bounded
    // epoch, then becomes read-mostly. This removes score-table and admission
    // overhead from long-running cold misses. A FIB revision restarts the
    // epoch; the decay model intentionally keeps learning continuously.
    if (alpha_ == 1.0 && tick_ > learningEpoch_) return result;

    const double value = policy_.observe(result.prefix, tick_);
    if (value < promote_) {
#ifdef FIBV2_VALIDATION
      lastAdmissionOutcome_ = AdmissionOutcome::BelowPromotion;
#endif
    } else {
#ifdef FIBV2_VALIDATION
      ++promotionCrossings_;
#endif
      if (policy_.eligibleForAdmission(result.prefix, value)) {
        admit(result);
      } else {
#ifdef FIBV2_VALIDATION
        ++cooldownRejections_;
        lastAdmissionOutcome_ = AdmissionOutcome::CooldownRejected;
#endif
      }
    }
    return result;
  }

  bool isCached(const std::string& prefix) const {
    return entries_.find(prefix) != entries_.end();
  }
  // Testing/metrics accessor: current decay-adjusted resident score for a
  // cached prefix, or -1.0 if the prefix is not resident.
  double residentScoreOf(const std::string& prefix) const {
    auto it = entries_.find(prefix);
    if (it == entries_.end()) return -1.0;
    return currentResident(*it->second);
  }
  uint64_t fastHits() const { return hits_; }
  uint64_t slowLookups() const { return slow_; }
  uint64_t evictions() const { return evictions_; }
#ifdef FIBV2_VALIDATION
  uint64_t promotionCrossings() const { return promotionCrossings_; }
  uint64_t cooldownRejections() const { return cooldownRejections_; }
  uint64_t victimRejections() const { return victimRejections_; }
  uint64_t admissions() const { return admissions_; }
  uint64_t replacementEvictions() const { return replacementEvictions_; }
  uint64_t coolingEvictions() const { return coolingEvictions_; }
  AdmissionOutcome lastAdmissionOutcome() const { return lastAdmissionOutcome_; }
#endif
  size_t cacheSize() const { return entries_.size(); }
  std::vector<std::string> residentPrefixes() const {
    std::vector<std::string> out;
    out.reserve(entries_.size());
    for (const auto& kv : entries_) out.push_back(kv.first);
    return out;
  }
  // Generic access to the concrete policy for policy-specific metrics
  // (e.g. BoundedExactPolicy::size()/capacity()/evictions(),
  // BoundedFrequencySketch::logicalBytes()/agingPasses(),
  // UnboundedCandidatePolicy::size()). No shared metric surface is imposed
  // here because the three policies have genuinely different notions of
  // "how big is Layer 2 right now" -- forcing a common metric API would
  // misrepresent at least one of them (see the CandidatePolicy interface
  // discussion above CandidateTable).
  const CandidatePolicy& candidatePolicy() const { return policy_; }
  void resetStats() {
    hits_ = slow_ = evictions_ = 0;
#ifdef FIBV2_VALIDATION
    promotionCrossings_ = cooldownRejections_ = victimRejections_ = 0;
    admissions_ = replacementEvictions_ = coolingEvictions_ = 0;
    lastAdmissionOutcome_ = AdmissionOutcome::None;
#endif
    policy_.resetStats();
  }
};

// Convenience aliases for the three Layer-2 policy instantiations.
using HotPrefixEngineB0 = HotPrefixEngine<UnboundedCandidatePolicy>;  // unbounded exact (oracle)
using HotPrefixEngineB1 = HotPrefixEngine<BoundedExactPolicy>;        // bounded exact table
using HotPrefixEngineB2 = HotPrefixEngine<BoundedFrequencySketch>;    // bounded frequency sketch

inline void populate(TrieFib& fib, HashFibEngine* hash=nullptr) {
  for(int a=0;a<64;++a)for(int b=0;b<16;++b){
    std::string p="/d"+std::to_string(a)+"/s"+std::to_string(b);
    int face=(a*16+b)%32;fib.insert(p,face);if(hash)hash->insert(p,face);
    if((a+b)%5==0){std::string q=p+"/deep/x";fib.insert(q,face+100);if(hash)hash->insert(q,face+100);}
  }
}

inline std::vector<std::string> workload(size_t n) {
  std::mt19937_64 g(0x4e444e7632ULL);std::vector<std::string>w;w.reserve(n);
  std::discrete_distribution<int> hot({55,20,10,6,4,3,2});std::uniform_int_distribution<int> cold(7,63),sub(0,15),coin(0,9);
  for(size_t i=0;i<n;++i){int a=(coin(g)<8)?hot(g):cold(g);int b=sub(g);std::string q="/d"+std::to_string(a)+"/s"+std::to_string(b);if((a+b)%5==0&&coin(g)<3)q+="/deep/x";q+="/content/"+std::to_string(i%1009);w.push_back(std::move(q));}
  return w;
}

inline bool regressionChecks() {
  TrieFib f; HashFibEngine h;
  f.insert("/a",1); h.insert("/a",1);
  f.insert("/a/b/c",2); h.insert("/a/b/c",2);
  if (!sameResult(f.lookup("/a/b/c/x"),h.lookup("/a/b/c/x"))) return false;

  HotPrefixEngineB1 hot(f,8,1,1.0,0.0);
  if (hot.lookup("/a/z").face != 1 || hot.lookup("/a/z/again").face != 1) return false;
  if (hot.lookup("/a/b/c/x").face != 2) return false;
  f.insert("/a/z/deep",3);
  if (hot.lookup("/a/z/deep/x").face != 3) return false;

  // B2 (sketch) sanity check: same scenario, must still return correct LPM
  // results regardless of the policy's estimation error (the
  // admission-policy independence theorem in practice).
  {
    TrieFib f2; f2.insert("/a",1); f2.insert("/a/b/c",2);
    HotPrefixEngineB2 sketch(f2,8,1,1.0,0.0);
    if (sketch.lookup("/a/z").face != 1) return false;
    if (sketch.lookup("/a/b/c/x").face != 2) return false;
    f2.insert("/a/z/deep",3);
    if (sketch.lookup("/a/z/deep/x").face != 3) return false;
  }

  // Overflow guard: k*capacity that cannot fit in size_t must throw, not
  // attempt an astronomical allocation.
  {
    bool threw = false;
    try {
      HotPrefixEngineB1 overflowing(f, std::numeric_limits<size_t>::max() - 1,
                                    1, 1.0, 0.0, std::numeric_limits<size_t>::max() - 1);
    } catch (const std::overflow_error&) {
      threw = true;
    }
    if (!threw) return false;
  }

  KimCacheEngine kim(f,8);
  if (kim.lookup("/a/z/deep/x").face != 3) return false;
  f.remove("/a/z/deep");
  if (kim.lookup("/a/z/deep/x").face != 1) return false;

  TrieFib pf;
  pf.insert("/com/google", 10);
  pf.insert("/com/google/map", 11);
  pf.insert("/com/amazon", 12);
  PbcEngine pbc(pf, 8);
  if (pbc.lookup("/com/google/earth").face != 10) return false;
  if (pbc.lookup("/com/google/earth/2").face != 10) return false;
  if (pbc.lookup("/com/google/map/1").face != 11) return false;
  if (pbc.lookup("/com/amazon/1").face != 12) return false;
  pf.insert("/com/google/earth/deep", 13);
  if (pbc.lookup("/com/google/earth/deep/1").face != 13) return false;
  return true;
}

template<class Engine> inline std::string extraStats(const Engine&) { return ""; }
inline std::string extraStats(const PbcEngine& e) {
  return " bitmap_positive=" + std::to_string(e.bitmapPositives()) +
         " false_cache_miss=" + std::to_string(e.falseMisses());
}

template<class Engine> inline int validateAndBench(const std::string&label,Engine&engine,const TrieFib&oracle,const std::vector<std::string>&w){
  size_t errors=0;uint64_t checksum=0;for(const auto&q:w){Result got=engine.lookup(q),want=oracle.lookup(q);if(!sameResult(got,want)&&++errors<=3)std::cerr<<"mismatch "<<q<<" got="<<got.prefix<<" expected="<<want.prefix<<"\n";checksum=checksum*1315423911u+static_cast<uint64_t>(got.face+2);}
  engine.resetStats();
  auto t0=std::chrono::steady_clock::now();for(const auto&q:w){auto r=engine.lookup(q);checksum^=static_cast<uint64_t>(r.face+2);}auto t1=std::chrono::steady_clock::now();
  double ns=std::chrono::duration<double,std::nano>(t1-t0).count()/w.size();
  std::cout<<label<<" queries="<<w.size()<<" errors="<<errors<<" ns/query="<<ns<<" fast_hits="<<engine.fastHits()<<" slow="<<engine.slowLookups()<<extraStats(engine)<<" checksum="<<checksum<<"\n";return errors?1:0;
}

inline int run(const std::string& mode,int argc,char**argv){
  if (!regressionChecks()) { std::cerr << "v2 regression check failed\n"; return 2; }
  size_t n=200000;if(argc>1)n=static_cast<size_t>(std::stoull(argv[1]));TrieFib fib;HashFibEngine hash;populate(fib,mode=="hash"?&hash:nullptr);auto w=workload(n);
  if(mode=="trie"){TrieEngine e(fib);return validateAndBench("trie_v2",e,fib,w);}
  if(mode=="hash")return validateAndBench("adaptive_hash_fib_v2",hash,fib,w);
  if(mode=="kim"){KimCacheEngine e(fib,512);return validateAndBench("kim2020_route_cache_v2",e,fib,w);}
  if(mode=="pbc"){PbcEngine e(fib,512);return validateAndBench("pbc_v2",e,fib,w);}
  if(mode=="hot"){HotPrefixEngineB1 e(fib,512,4,1.0,0.0);return validateAndBench("hot_prefix_cache_v2",e,fib,w);}
  if(mode=="decay"){HotPrefixEngineB1 e(fib,512,4,.995,.25);return validateAndBench("hot_prefix_cache_decay_v2",e,fib,w);}
  if(mode=="sketch"){HotPrefixEngineB2 e(fib,512,4,1.0,0.0);return validateAndBench("hot_prefix_cache_sketch_v2",e,fib,w);}
  HotPrefixEngineB0 e(fib,512,4,1.0,0.0);return validateAndBench("hot_prefix_cache_unbounded_v2",e,fib,w);
}

} // namespace fibv2
