#!/usr/bin/env python3
"""Dataset generator for the Hot-prefix v3 evaluation.

FIB families
  synth  : the v2 synthetic namespace (identical base prefixes for nest=0)
           plus a fraction `nest` of aggregate prefixes obtained by
           truncating base prefixes, which creates registered ancestors and
           non-empty branch guards.
  domain : reversed domain names from a public top-1M ranked list
           (e.g. google.com -> /com/google); nesting is whatever the list has.

Request names: target prefix drawn by popularity, then
  - for a target with registered descendants, with probability `rho` the
    name continues into a guarded branch (a random descendant truncated at a
    random depth), otherwise it continues with an unguarded component;
  - a suffix: `object` (/object/<i mod 257>, as in v2) or `ts` (a unique,
    increasing timestamp component per request).

Workloads: stationary (Zipf alpha), phaseshift (popularity re-ranked at N/2),
scan (Zipf, one-off uniform scan for the middle half, Zipf), churn (70% one-off
uniform, 30% Zipf 1.5).
"""
import argparse
import json
import zlib
from pathlib import Path

import numpy as np


def base_prefix(index: int) -> str:
    """Identical to generate_ndn_datasets.py::prefix() of the v2 code."""
    depth = 3 + zlib.crc32(str(index).encode("ascii")) % 8
    middle = [
        "service",
        f"region{(index // 1024) % 16}",
        f"site{(index // 16384) % 32}",
        f"app{index % 16}",
        f"collection{(index // 16) % 64}",
        f"version{(index // 64) % 8}",
        f"shard{(index // 8) % 32}",
    ]
    comps = ["ndn", f"d{index % 1024}"] + middle[: depth - 3] + [f"s{index}"]
    return "/" + "/".join(comps)


def comps_of(p):
    return p[1:].split("/")


def build_synth(n: int, nest: float, seed: int):
    rng = np.random.default_rng(seed)
    n_agg = int(round(n * nest))
    n_base = n - n_agg
    prefixes = [base_prefix(i) for i in range(n_base)]
    registered = set(prefixes)
    source = {}  # aggregate -> one base descendant
    attempts = 0
    aggs = []
    while len(aggs) < n_agg and attempts < 50 * max(1, n_agg):
        attempts += 1
        b = int(rng.integers(n_base))
        c = comps_of(prefixes[b])
        if len(c) < 3:
            continue
        k = int(rng.integers(2, len(c)))  # 2..len-1
        a = "/" + "/".join(c[:k])
        if a in registered:
            continue
        registered.add(a)
        source[a] = prefixes[b]
        aggs.append(a)
    allp = prefixes + aggs
    return allp, source


def load_domains(path: str, n: int):
    out, seen = [], set()
    with open(path, encoding="utf-8", errors="ignore") as f:
        for line in f:
            d = line.strip().lower().strip(".")
            if not d or "/" in d or " " in d:
                continue
            labels = [x for x in d.split(".") if x]
            if len(labels) < 2:
                continue
            p = "/" + "/".join(reversed(labels))
            if p in seen:
                continue
            seen.add(p)
            out.append(p)
            if len(out) >= n:
                break
    return out


def descendants_index(prefixes):
    """Map each prefix with registered descendants to a list of descendants."""
    reg = set(prefixes)
    desc = {}
    for p in prefixes:
        c = comps_of(p)
        for k in range(1, len(c)):
            a = "/" + "/".join(c[:k])
            if a in reg:
                desc.setdefault(a, []).append(p)
    return desc


def zipf_ranks(n_items, count, alpha, rng):
    w = 1.0 / np.power(np.arange(1, n_items + 1, dtype=np.float64), alpha)
    cdf = np.cumsum(w)
    cdf /= cdf[-1]
    return np.minimum(np.searchsorted(cdf, rng.random(count)), n_items - 1)


def make_names(prefixes, desc, targets, rho, suffix, rng, start_index, tag):
    out = []
    coin = rng.random(len(targets))
    pick = rng.integers(0, 1 << 30, len(targets))
    for j, t in enumerate(targets):
        i = start_index + j
        p = prefixes[t]
        ds = desc.get(p)
        if ds is not None and coin[j] < rho:
            d = ds[pick[j] % len(ds)]
            cp, cd = comps_of(p), comps_of(d)
            k = len(cp) + 1 + (pick[j] >> 8) % (len(cd) - len(cp))
            name = "/" + "/".join(cd[:k])
        elif ds is not None:
            name = p + f"/{tag}{pick[j] % 50}"
        else:
            name = p
        if suffix == "object":
            name += f"/object/{i % 257}"
        else:
            name += f"/ts/{1700000000000 + i}"
        out.append(name)
    return out


def workload(prefixes, desc, kind, count, alpha, rho, suffix, seed, ranked):
    rng = np.random.default_rng(seed)
    n = len(prefixes)

    def order(s):
        if ranked and s == 0:
            return np.arange(n)
        return np.random.default_rng(seed * 7919 + s).permutation(n)

    def zipf_targets(cnt, a, s):
        return order(s)[zipf_ranks(n, cnt, a, rng)]

    if kind == "stationary":
        t = zipf_targets(count, alpha, 0)
    elif kind == "phaseshift":
        h = count // 2
        t = np.concatenate([zipf_targets(h, alpha, 0), zipf_targets(count - h, alpha, 1)])
    elif kind == "scan":
        a, m = count // 4, count // 2
        t = np.concatenate([zipf_targets(a, alpha, 0), rng.integers(0, n, m),
                            zipf_targets(count - a - m, alpha, 0)])
    elif kind == "churn":
        broad = rng.random(count) < 0.7
        t = np.empty(count, dtype=np.int64)
        t[broad] = rng.integers(0, n, int(broad.sum()))
        t[~broad] = zipf_targets(int((~broad).sum()), 1.5, 0)
    else:
        raise ValueError(kind)
    return make_names(prefixes, desc, t.tolist(), rho, suffix, rng, 0, "x")


def nest_stats(prefixes, desc):
    depths = np.array([len(comps_of(p)) for p in prefixes])
    return {
        "prefixes": len(prefixes),
        "with_descendants": len(desc),
        "fraction_with_descendants": len(desc) / len(prefixes),
        "depth_min": int(depths.min()), "depth_max": int(depths.max()),
        "depth_mean": float(depths.mean()),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--family", choices=["synth", "domain"], required=True)
    ap.add_argument("--fib-size", type=int, required=True)
    ap.add_argument("--nest", type=float, default=0.0)
    ap.add_argument("--domains", default="")
    ap.add_argument("--fib-seed", type=int, default=12345)
    ap.add_argument("--out-fib", required=True)
    ap.add_argument("--queries", nargs="*", default=[],
                    help="specs kind:alpha:rho:suffix:count:seed:path")
    args = ap.parse_args()

    if args.family == "synth":
        prefixes, _ = build_synth(args.fib_size, args.nest, args.fib_seed)
        ranked = False
    else:
        prefixes = load_domains(args.domains, args.fib_size)
        ranked = True  # popularity follows the list's rank order
    desc = descendants_index(prefixes)
    out = Path(args.out_fib)
    if not out.exists():
        with out.open("w") as f:
            f.write("prefix,face\n")
            for i, p in enumerate(prefixes):
                f.write(f"{p},{i % 4096}\n")
        stats = nest_stats(prefixes, desc)
        stats.update(family=args.family, nest_param=args.nest, fib_seed=args.fib_seed)
        Path(str(out) + ".json").write_text(json.dumps(stats, indent=1))
    for spec in args.queries:
        kind, alpha, rho, suffix, count, seed, path = spec.split(":", 6)
        names = workload(prefixes, desc, kind, int(count), float(alpha), float(rho), suffix,
                         int(seed), ranked)
        with open(path, "w") as f:
            f.write("query\n")
            f.write("\n".join(names))
            f.write("\n")


if __name__ == "__main__":
    main()
