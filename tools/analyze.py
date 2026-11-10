#!/usr/bin/env python3
"""Statistics, tables and figures for the v3 experiments.

Paired design: within one run every engine processes the same stream in a
random order, so each engine is compared with the hash FIB of the SAME run:
    R = 100 * (T_hash - T_engine) / T_hash      (positive = faster than hash)
Seed-level value = median over runs; config estimate = mean over seeds with a
95% percentile bootstrap over seeds (20,000 resamples) and an exact two-sided
sign test over seeds.
"""
import collections
import csv
import json
import math
import statistics as st
import sys
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

import os
ROOT = Path(__file__).resolve().parent.parent
RES = Path(os.environ.get("FIBV3_RESULTS", ROOT / "results" / "development"))
FIG = Path(os.environ.get("FIBV3_FIGURES", RES / "figures"))
TAB = Path(os.environ.get("FIBV3_TABLES", RES / "tables"))
FIG.mkdir(parents=True, exist_ok=True)
TAB.mkdir(parents=True, exist_ok=True)

# Fixed engine -> colour (reference categorical palette, fixed order) and marker.
STYLE = {
    "hot_b2h": ("#2a78d6", "o", "Hot-prefix B2h"),
    "pbc": ("#eb6834", "s", "PBC"),
    "kim": ("#1baf7a", "^", "Kim (128-bit)"),
    "full": ("#eda100", "D", "Full-name cache"),
    "hash2s": ("#e87ba4", "v", "Two-stage hash"),
    "hot_b1": ("#008300", "P", "Hot-prefix B1"),
    "hot_b2": ("#6250d6", "X", "Hot-prefix B2"),
    "hot_all": ("#e34948", "*", "Prefix cache, admit-all"),
    "kim8": ("#1baf7a", "<", "Kim (8-bit)"),
    "hash": ("#52514e", "", "Hash FIB"),
}
LABEL = {k: v[2] for k, v in STYLE.items()}
LABEL.update({"hpx:L=0/S=16/INC=1": "HP-A incremental", "hpb": "HP-B", "hpb:AB=0": "HP-B without bypass", "hpb:U=0/TF=0": "HP-B, bypass only",
              "hpb_2s": "HP-B over two-stage", "hash#aa": "Hash FIB (A/A copy)",
              "hpx": "HP-A (LB, cutoff 64K)", "hpx:LB=0": "HP-A without LB", "hpx:L=16384": "HP-A, cutoff 16K",
              "hpx:L=0/S=4": "HP-A, sample 1/4", "hpx:L=0/S=8": "HP-A, sample 1/8", "hpx:L=0/S=16": "HP-A, sample 1/16",
              "hpx:L=0/S=32": "HP-A, sample 1/32",
              "hot_b2h_2s": "B2h over two-stage", "hot_b2h:L=0/S=4": "B2h, sampled 1/4", "hot_b2h:L=0/S=8": "B2h, sampled 1/8",
              "hot_b2h:L=0/S=16": "B2h, sampled 1/16", "hot_b2h:L=0": "B2h, no cutoff", "hot_b2h_decay": "B2h, decay", "hot_b1_decay": "B1, decay",
              "hot_b2h:L=16384": "L=16K", "hot_b2h:L=65536": "L=64K", "hot_b2h:L=262144": "L=256K",
              "trie": "Trie", "v2_trie": "Trie (v2)", "v2_hash": "Hash (v2)", "v2_hot_b1": "Hot-prefix B1 (v2)",
              "v2_hot_b2": "Hot-prefix B2 (v2)"})

plt.rcParams.update({
    "font.family": "DejaVu Sans", "font.size": 8, "axes.titlesize": 8, "axes.labelsize": 8, "legend.fontsize": 7,
    "xtick.labelsize": 7, "ytick.labelsize": 7, "axes.spines.top": False, "axes.spines.right": False,
    "axes.grid": True, "grid.color": "#e4e3df", "grid.linewidth": 0.6, "axes.edgecolor": "#8a8984",
    "lines.linewidth": 1.6, "lines.markersize": 4.5, "pdf.fonttype": 42, "ps.fonttype": 42,
    "savefig.bbox": "tight", "savefig.dpi": 200,
})
W1, W2 = 3.5, 7.16  # IEEE/Elsevier single and double column widths (inches)


def rows(path):
    with open(path) as f:
        r = csv.reader(f)
        hdr = next(r, None)
        if hdr is None:
            return
        for x in r:
            while len(x) > len(hdr):
                x = [x[0] + ";" + x[1]] + x[2:]
            d = dict(zip(hdr, x))
            for k in list(d):
                if k not in ("tag", "engine"):
                    try:
                        d[k] = float(d[k])
                    except ValueError:
                        pass
            yield d


def parse_tag(tag):
    out = {}
    for kv in tag.split(";"):
        if "=" in kv:
            k, v = kv.split("=", 1)
            out[k] = v
    return out


def load(exp):
    out = []
    for p in sorted((RES / exp).glob("*_runs.csv")):
        for r in rows(p):
            r.update(parse_tag(r["tag"]))
            r["file"] = p.stem
            out.append(r)
    return out


def bootstrap(vals, n=20000, seed=7):
    v = np.asarray(vals, dtype=float)
    if len(v) < 2:
        return (float(v.mean()), float("nan"), float("nan"))
    rng = np.random.default_rng(seed)
    m = v[rng.integers(0, len(v), (n, len(v)))].mean(axis=1)
    return float(v.mean()), float(np.percentile(m, 2.5)), float(np.percentile(m, 97.5))


def sign_test(vals):
    pos = sum(1 for x in vals if x > 0)
    neg = sum(1 for x in vals if x < 0)
    n = pos + neg
    k = min(pos, neg)
    p = min(1.0, 2 * sum(math.comb(n, i) for i in range(k + 1)) / 2 ** n) if n else 1.0
    return pos, n, p


def paired(data, keys, metric="mean_ns", base="hash"):
    """Return {config_key: {engine: dict(stats)}} for rows grouped by keys."""
    groups = collections.defaultdict(list)
    for r in data:
        groups[tuple(r.get(k) for k in keys)].append(r)
    out = {}
    for g, rs in groups.items():
        byrun = collections.defaultdict(dict)
        for r in rs:
            byrun[(r["seed"], r["run"])][r["engine"]] = r
        engines = sorted({r["engine"] for r in rs})
        res = {}
        for e in engines:
            seed_red = collections.defaultdict(list)
            seed_abs = collections.defaultdict(list)
            seed_hit = collections.defaultdict(list)
            for (s, run), d in byrun.items():
                if e in d:
                    seed_abs[s].append(d[e][metric])
                    seed_hit[s].append(d[e]["hit_ratio"])
                    if base in d and d[base][metric] > 0:
                        seed_red[s].append(100.0 * (d[base][metric] - d[e][metric]) / d[base][metric])
            red = [st.median(v) for v in seed_red.values()]
            ab = [st.median(v) for v in seed_abs.values()]
            mean, lo, hi = bootstrap(red) if red else (float("nan"),) * 3
            pos, n, p = sign_test(red) if red else (0, 0, 1)
            res[e] = dict(red=mean, lo=lo, hi=hi, pos=pos, n=n, p=p, abs=float(np.mean(ab)) if ab else float("nan"),
                          hit=float(np.mean([st.median(v) for v in seed_hit.values()])) if seed_hit else float("nan"),
                          seeds=len(red), runs=sum(len(v) for v in seed_red.values()))
        out[g] = res
    return out


def fmt_red(s):
    if not s or math.isnan(s["red"]):
        return "--"
    if math.isnan(s["lo"]):
        return f"{s['red']:+.1f}"
    return f"{s['red']:+.1f} [{s['lo']:+.1f}, {s['hi']:+.1f}]"


def errorbar(ax, xs, stats, e, offset=0.0, logx=False):
    c, mk, lab = STYLE.get(e, ("#52514e", "o", e))
    x = np.asarray(xs, dtype=float)
    if logx:
        x = x * (1 + offset)
    else:
        x = x + offset
    y = np.array([s["red"] for s in stats])
    lo = np.array([s["lo"] for s in stats])
    hi = np.array([s["hi"] for s in stats])
    ax.plot(x, y, color=c, marker=mk, label=LABEL.get(e, e), zorder=3, linestyle="--" if e == "kim8" else "-")
    ok = ~np.isnan(lo)
    ax.vlines(x[ok], lo[ok], hi[ok], color=c, linewidth=1.0, zorder=2)


SUMMARY = {}


# ----------------------------------------------------------------------------
def an_paper():
    d = load("paper")
    if not d:
        return
    P = paired(d, ["tag"])[("paper-dataset",)]
    order = ["v2_trie", "v2_hash", "v2_hot_b1", "v2_hot_b2", "trie", "hash", "pbc", "kim", "hot_b1", "hot_b2",
             "hot_b2h"]
    with open(TAB / "paper_dataset.md", "w") as f:
        f.write("| Engine | Mean latency (ns) | Reduction vs v3 hash (%) [95% CI] | Hit ratio |\n|---|---|---|---|\n")
        for e in order:
            if e in P:
                s = P[e]
                f.write(f"| {LABEL.get(e, e)} | {s['abs']:.0f} | {fmt_red(s)} | {s['hit']:.3f} |\n")
    # v2 internal comparison as in the manuscript: B2 vs v2 hash
    v2 = paired(d, ["tag"], base="v2_hash")[("paper-dataset",)]
    SUMMARY["paper"] = {e: {"abs_ns": P[e]["abs"], "red_vs_v3hash": P[e]["red"], "hit": P[e]["hit"]} for e in P}
    SUMMARY["paper"]["v2_hot_b2_vs_v2_hash"] = v2.get("v2_hot_b2", {}).get("red")
    fig, ax = plt.subplots(figsize=(W1, 2.1))
    names = [e for e in order if e in P]
    vals = [P[e]["abs"] for e in names]
    cols = ["#8a8984" if e.startswith("v2_") else STYLE.get(e, ("#52514e",))[0] for e in names]
    ax.barh(range(len(names)), vals, color=cols, height=0.7)
    ax.set_yticks(range(len(names)))
    ax.set_yticklabels([LABEL.get(e, e) for e in names])
    ax.invert_yaxis()
    ax.set_xlabel("Mean lookup latency (ns), manuscript dataset")
    for i, v in enumerate(vals):
        ax.text(v + 15, i, f"{v:.0f}", va="center", fontsize=6.5, color="#52514e")
    ax.grid(axis="y", visible=False)
    fig.savefig(FIG / "fig_paper_dataset_v2_vs_v3.pdf")
    plt.close(fig)


def an_main():
    d = load("main")
    if not d:
        return
    P = paired(d, ["n", "alpha"])
    P_last = paired(d, ["n", "alpha"], metric="last_ns")
    P_first = paired(d, ["n", "alpha"], metric="first_ns")
    ns = sorted({int(k[0]) for k in P})
    alphas = sorted({k[1] for k in P}, key=float)
    engines = ["hot_b2h", "hot_b2", "hot_b1", "hot_all", "pbc", "kim", "full", "hash2s"]
    with open(TAB / "main.md", "w") as f:
        f.write("Reduction of mean latency vs hash FIB, % [95% CI over seeds]; hash latency in ns. "
                "Unique timestamp suffixes, nest=0.3, C=1024, 300K requests.\n\n")
        f.write("| FIB | alpha | hash ns | " + " | ".join(LABEL[e] for e in engines) + " | B2h hit | B2h steady-state |\n")
        f.write("|" + "---|" * (len(engines) + 5) + "\n")
        for n in ns:
            for a in alphas:
                k = (str(n), a)
                if k not in P:
                    continue
                cells = [fmt_red(P[k].get(e)) for e in engines]
                f.write(f"| {n:,} | {a} | {P[k]['hash']['abs']:.0f} | " + " | ".join(cells) +
                        f" | {P[k]['hot_b2h']['hit']:.3f} | {fmt_red(P_last[k].get('hot_b2h'))} |\n")
    SUMMARY["main"] = {f"{k[0]}|{k[1]}": {e: {kk: v[kk] for kk in ("red", "lo", "hi", "pos", "n", "abs", "hit")}
                                          for e, v in P[k].items()} for k in P}
    SUMMARY["main_last"] = {f"{k[0]}|{k[1]}": {e: {kk: v[kk] for kk in ("red", "lo", "hi", "pos", "n")}
                                               for e, v in P_last[k].items()} for k in P_last}
    SUMMARY["main_first"] = {f"{k[0]}|{k[1]}": {e: {kk: v[kk] for kk in ("red", "lo", "hi", "pos", "n")}
                                                for e, v in P_first[k].items()} for k in P_first}
    # Figure: reduction vs FIB size, one panel per alpha
    fig, axes = plt.subplots(1, len(alphas), figsize=(W2, 2.3), sharey=True)
    show = ["hot_b2h", "hot_b2", "pbc", "kim", "full"]
    for ax, a in zip(np.atleast_1d(axes), alphas):
        for i, e in enumerate(show):
            stats = [P[(str(n), a)][e] for n in ns]
            errorbar(ax, ns, stats, e, offset=(i - 2) * 0.04, logx=True)
        ax.axhline(0, color="#52514e", linewidth=0.9, linestyle="--")
        ax.set_xscale("log")
        ax.set_xticks(ns)
        ax.set_xticklabels([f"{n // 1000}K" if n < 10 ** 6 else f"{n // 10 ** 6}M" for n in ns])
        ax.set_title(f"Zipf α = {a}")
        ax.set_xlabel("FIB size (prefixes)")
    np.atleast_1d(axes)[0].set_ylabel("Latency reduction vs hash FIB (%)")
    np.atleast_1d(axes)[-1].legend(loc="lower right", frameon=False)
    fig.savefig(FIG / "fig_main_reduction_vs_fibsize.pdf")
    plt.close(fig)
    # Figure: absolute latency, alpha panels
    fig, axes = plt.subplots(1, len(alphas), figsize=(W2, 2.3), sharey=True)
    for ax, a in zip(np.atleast_1d(axes), alphas):
        for e in ["hash"] + show:
            c, mk, lab = STYLE[e]
            y = [P[(str(n), a)][e]["abs"] for n in ns]
            ax.plot(ns, y, color=c, marker=mk or None, label=lab, linestyle="--" if e == "hash" else "-")
        ax.set_xscale("log")
        ax.set_xticks(ns)
        ax.set_xticklabels([f"{n // 1000}K" if n < 10 ** 6 else f"{n // 10 ** 6}M" for n in ns])
        ax.set_title(f"Zipf α = {a}")
        ax.set_xlabel("FIB size (prefixes)")
    np.atleast_1d(axes)[0].set_ylabel("Mean lookup latency (ns)")
    np.atleast_1d(axes)[0].legend(loc="upper left", frameon=False)
    fig.savefig(FIG / "fig_main_latency_vs_fibsize.pdf")
    plt.close(fig)


def an_timeline():
    """Window latency for the main configuration (2M, alpha 1.2)."""
    files = sorted((RES / "main").glob("n2000000_a1.2_s*_windows.csv"))
    if not files:
        return
    per = collections.defaultdict(lambda: collections.defaultdict(list))  # engine -> seed -> list of arrays
    hits = collections.defaultdict(lambda: collections.defaultdict(list))
    for p in files:
        cur = collections.defaultdict(dict)
        for r in rows(p):
            cur[(r["engine"], r["seed"], r["run"])][int(r["window"])] = (r["latency_ns"], r["hits"])
        for (e, s, run), w in cur.items():
            ks = sorted(w)
            per[e][s].append(np.array([w[k][0] for k in ks]))
            hits[e][s].append(np.array([w[k][1] for k in ks]))
    engines = ["hash", "hot_b2h", "hot_b2", "pbc", "kim"]
    fig, axes = plt.subplots(2, 1, figsize=(W1, 3.6), sharex=True, gridspec_kw={"height_ratios": [2, 1]})
    for e in engines:
        if e not in per:
            continue
        seeds = sorted(per[e])
        mats = np.array([np.median(np.vstack(per[e][s]), axis=0) for s in seeds])  # seed x window
        k = 10  # smooth over 10 windows (10K requests) for readability
        sm = np.array([np.convolve(m, np.ones(k) / k, mode="valid") for m in mats])
        x = (np.arange(sm.shape[1]) + k) * 1000
        rng = np.random.default_rng(3)
        boots = sm[rng.integers(0, len(seeds), (4000, len(seeds)))].mean(axis=1)
        c, mk, lab = STYLE[e]
        axes[0].plot(x, sm.mean(axis=0), color=c, label=lab, linestyle="--" if e == "hash" else "-", linewidth=1.3)
        axes[0].fill_between(x, np.percentile(boots, 2.5, axis=0), np.percentile(boots, 97.5, axis=0), color=c,
                             alpha=0.18, linewidth=0)
        if e != "hash":
            hm = np.array([np.median(np.vstack(hits[e][s]), axis=0) for s in seeds]) / 1000.0
            hs = np.array([np.convolve(m, np.ones(k) / k, mode="valid") for m in hm])
            axes[1].plot(x, hs.mean(axis=0), color=c, linewidth=1.3)
    axes[0].axvline(65536, color="#8a8984", linewidth=0.8, linestyle=":")
    lo_, hi_ = axes[0].get_ylim()
    axes[0].text(68000, lo_ + 0.04 * (hi_ - lo_), "learning cutoff (L = 65,536)", fontsize=6.5, color="#52514e", va="bottom")
    axes[0].set_ylabel("Window latency (ns)")
    axes[0].legend(frameon=False, ncol=3, loc="lower center", bbox_to_anchor=(0.5, 1.0), fontsize=6.5)
    axes[1].set_ylabel("Hit ratio")
    axes[1].set_xlabel("Requests processed")
    axes[1].set_ylim(0, 1)
    fig.savefig(FIG / "fig_timeline_2M_a1.2.pdf")
    plt.close(fig)


def an_nest():
    d = load("nest")
    if not d:
        return
    for r in d:
        r["fp_rate"] = (r["guard_rejects"] + r["filter_pos"]) / r["queries"]
    P = paired(d, ["nest", "alpha"])
    nests = sorted({k[0] for k in P}, key=float)
    alphas = sorted({k[1] for k in P}, key=float)
    rates = collections.defaultdict(list)
    for r in d:
        rates[(r["nest"], r["alpha"], r["engine"])].append(r["fp_rate"])
    with open(TAB / "nest.md", "w") as f:
        f.write("Nesting sweep (500K FIB, timestamp suffix). Reduction vs hash % [95% CI]; "
                "guard/filter test failures per lookup.\n\n| nest | alpha | hash ns | B2h | PBC | Kim 128b | Kim 8b | "
                "B2h guard rejects/lookup | PBC bitmap pos./lookup | Kim128 filter pos./lookup | Kim8 filter pos./lookup |\n"
                "|---|---|---|---|---|---|---|---|---|---|---|\n")
        for nst in nests:
            for a in alphas:
                k = (nst, a)
                g = lambda e: st.mean(rates[(nst, a, e)]) if rates[(nst, a, e)] else float("nan")
                f.write(f"| {nst} | {a} | {P[k]['hash']['abs']:.0f} | {fmt_red(P[k].get('hot_b2h'))} | "
                        f"{fmt_red(P[k].get('pbc'))} | {fmt_red(P[k].get('kim'))} | {fmt_red(P[k].get('kim8'))} | "
                        f"{g('hot_b2h'):.4f} | {g('pbc'):.4f} | {g('kim'):.4f} | {g('kim8'):.4f} |\n")
    SUMMARY["nest"] = {f"{k[0]}|{k[1]}": {e: {kk: v[kk] for kk in ("red", "lo", "hi", "hit")} for e, v in P[k].items()}
                       for k in P}
    SUMMARY["nest_rates"] = {f"{k[0]}|{k[1]}|{k[2]}": st.mean(v) for k, v in rates.items()}
    fig, axes = plt.subplots(1, 2, figsize=(W2, 2.2))
    a = "1.2" if "1.2" in alphas else alphas[-1]
    x = [float(n) for n in nests]
    for i, e in enumerate(["hot_b2h", "pbc", "kim", "kim8"]):
        errorbar(axes[0], x, [P[(n, a)][e] for n in nests], e, offset=(i - 1.5) * 0.008)
        c, mk, lab = STYLE[e]
        axes[1].plot(x, [st.mean(rates[(n, a, e)]) for n in nests], color=c, marker=mk, label=LABEL[e],
                     linestyle="--" if e == "kim8" else "-")
    axes[0].axhline(0, color="#52514e", linewidth=0.9, linestyle="--")
    axes[0].set_xlabel("Fraction of FIB entries that are aggregates")
    axes[0].set_ylabel("Latency reduction vs hash (%)")
    axes[0].set_title(f"500K FIB, α = {a}")
    axes[1].set_xlabel("Fraction of FIB entries that are aggregates")
    axes[1].set_ylabel("Cached prefix rejected per lookup")
    axes[1].set_title("Guard / filter rejections")
    axes[1].legend(frameon=False)
    fig.savefig(FIG / "fig_nesting.pdf")
    plt.close(fig)


def an_capacity():
    d = load("capacity")
    if not d:
        return
    P = paired(d, ["alpha", "C"])
    alphas = sorted({k[0] for k in P}, key=float)
    cs = sorted({int(k[1]) for k in P})
    with open(TAB / "capacity.md", "w") as f:
        f.write("Capacity sweep (2M FIB). Reduction vs hash % [95% CI] and hit ratio.\n\n| alpha | C | B2h | B2h hit | B2 | PBC | PBC hit | Kim |\n|---|---|---|---|---|---|---|---|\n")
        for a in alphas:
            for c in cs:
                k = (a, str(c))
                f.write(f"| {a} | {c} | {fmt_red(P[k]['hot_b2h'])} | {P[k]['hot_b2h']['hit']:.3f} | {fmt_red(P[k]['hot_b2'])} | "
                        f"{fmt_red(P[k]['pbc'])} | {P[k]['pbc']['hit']:.3f} | {fmt_red(P[k]['kim'])} |\n")
    SUMMARY["capacity"] = {f"{k[0]}|{k[1]}": {e: {kk: v[kk] for kk in ("red", "lo", "hi", "hit")} for e, v in P[k].items()}
                           for k in P}
    fig, axes = plt.subplots(1, 2, figsize=(W2, 2.2))
    for ai, a in enumerate(alphas):
        ls = "-" if ai == len(alphas) - 1 else ":"
        for e in ["hot_b2h", "pbc", "kim"]:
            c, mk, lab = STYLE[e]
            axes[0].plot(cs, [P[(a, str(cc))][e]["red"] for cc in cs], color=c, marker=mk, linestyle=ls,
                         label=f"{lab}, α={a}")
            axes[1].plot(cs, [P[(a, str(cc))][e]["hit"] for cc in cs], color=c, marker=mk, linestyle=ls)
    for ax in axes:
        ax.set_xscale("log", base=2)
        ax.set_xticks(cs)
        ax.set_xticklabels([str(c) for c in cs])
        ax.set_xlabel("Cache capacity C (entries)")
    axes[0].axhline(0, color="#52514e", linewidth=0.9, linestyle="--")
    axes[0].set_ylabel("Latency reduction vs hash (%)")
    axes[1].set_ylabel("Hit ratio")
    axes[0].legend(frameon=False, fontsize=6)
    fig.savefig(FIG / "fig_capacity.pdf")
    plt.close(fig)


def bars(exp, key, engines, fname, title, keyorder=None, ylabel="Latency reduction vs hash (%)", metric="mean_ns"):
    d = load(exp)
    if not d:
        return None
    P = paired(d, [key], metric=metric)
    ks = keyorder or sorted(k[0] for k in P)
    ks = [k for k in ks if (k,) in P]
    fig, ax = plt.subplots(figsize=(W2 if len(ks) * len(engines) > 12 else W1, 2.2))
    wdt = 0.8 / len(engines)
    cols = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"]
    for i, e in enumerate(engines):
        y = [P[(k,)][e]["red"] if e in P[(k,)] else np.nan for k in ks]
        lo = [P[(k,)][e]["lo"] if e in P[(k,)] else np.nan for k in ks]
        hi = [P[(k,)][e]["hi"] if e in P[(k,)] else np.nan for k in ks]
        x = np.arange(len(ks)) + (i - (len(engines) - 1) / 2) * wdt
        c = STYLE[e][0] if e in STYLE else cols[i % len(cols)]
        ax.bar(x, y, width=wdt * 0.92, color=c, label=LABEL.get(e, e))
        yerr = np.array([np.array(y) - np.array(lo), np.array(hi) - np.array(y)])
        ax.errorbar(x, y, yerr=yerr, fmt="none", ecolor="#52514e", elinewidth=0.8, capsize=1.5)
    ax.axhline(0, color="#52514e", linewidth=0.9)
    ax.set_xticks(range(len(ks)))
    ax.set_xticklabels(ks)
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.grid(axis="x", visible=False)
    ax.legend(frameon=False, fontsize=6, ncol=2)
    fig.savefig(FIG / fname)
    plt.close(fig)
    return P


def an_dynamics():
    eng = ["hot_b2h", "hot_b2h:L=0", "hot_b2h_decay", "hot_b1", "hot_b1_decay", "pbc"]
    P = bars("dynamics", "workload", eng, "fig_dynamics.pdf", "2M FIB, α = 1.0, C = 1024",
             keyorder=["stationary", "phaseshift", "scan", "churn"])
    if not P:
        return
    with open(TAB / "dynamics.md", "w") as f:
        f.write("Non-stationary workloads (2M FIB, alpha=1.0, C=1024). Reduction vs hash % [95% CI]; (hit ratio).\n\n| workload | hash ns | " +
                " | ".join(LABEL.get(e, e) for e in eng) + " |\n|" + "---|" * (len(eng) + 2) + "\n")
        for k in ["stationary", "phaseshift", "scan", "churn"]:
            if (k,) not in P:
                continue
            f.write(f"| {k} | {P[(k,)]['hash']['abs']:.0f} | " + " | ".join(
                f"{fmt_red(P[(k,)][e])} ({P[(k,)][e]['hit']:.2f})" for e in eng) + " |\n")
    SUMMARY["dynamics"] = {k[0]: {e: {kk: v[kk] for kk in ("red", "lo", "hi", "hit")} for e, v in P[k].items()} for k in P}
    eng12 = ["hot_b2h", "hot_b2h:L=0", "hot_b2h_decay", "hot_b2", "pbc"]
    P12 = bars("dynamics12", "workload", eng12, "fig_dynamics_a1.2.pdf", "2M FIB, α = 1.2, C = 1024",
               keyorder=["stationary", "phaseshift", "scan", "churn"])
    if P12:
        with open(TAB / "dynamics_a1.2.md", "w") as f:
            f.write("Non-stationary workloads (2M FIB, alpha=1.2, C=1024). Reduction vs hash % [95% CI]; (hit ratio).\n\n| workload | hash ns | " +
                    " | ".join(LABEL.get(e, e) for e in eng12) + " |\n|" + "---|" * (len(eng12) + 2) + "\n")
            for k in ["stationary", "phaseshift", "scan", "churn"]:
                if (k,) in P12:
                    f.write(f"| {k} | {P12[(k,)]['hash']['abs']:.0f} | " + " | ".join(
                        f"{fmt_red(P12[(k,)][e])} ({P12[(k,)][e]['hit']:.2f})" for e in eng12) + " |\n")
        SUMMARY["dynamics12"] = {k[0]: {e: {kk: v[kk] for kk in ("red", "lo", "hi", "hit")} for e, v in P12[k].items()} for k in P12}


def an_cutoff():
    d = load("cutoff")
    if not d:
        return
    eng = ["hot_b2h:L=16384", "hot_b2h:L=65536", "hot_b2h:L=262144", "hot_b2h:L=0"]
    with open(TAB / "cutoff.md", "w") as f:
        f.write("Learning cutoff L (2M FIB). Reduction vs hash % [95% CI] over the whole stream, the first 50K and the last 100K requests.\n\n"
                "| alpha | L | whole | first 50K | last 100K | hit |\n|---|---|---|---|---|---|\n")
        out = {}
        for metric, name in [("mean_ns", "whole"), ("first_ns", "first"), ("last_ns", "last")]:
            out[name] = paired(d, ["alpha"], metric=metric)
        for a in sorted({k[0] for k in out["whole"]}, key=float):
            for e in eng:
                f.write(f"| {a} | {LABEL[e]} | {fmt_red(out['whole'][(a,)][e])} | {fmt_red(out['first'][(a,)][e])} | "
                        f"{fmt_red(out['last'][(a,)][e])} | {out['whole'][(a,)][e]['hit']:.3f} |\n")
    SUMMARY["cutoff"] = {n: {k[0]: {e: {kk: v[kk] for kk in ("red", "lo", "hi", "hit")} for e, v in P[k].items()} for k in P}
                         for n, P in out.items()}


def an_domain():
    d = load("domain")
    if not d:
        return
    P = paired(d, ["alpha", "suffix"])
    eng = ["hot_b2h", "hot_b2", "pbc", "kim", "full", "hash2s"]
    with open(TAB / "domain.md", "w") as f:
        f.write("Domain-name FIB (1M reversed domains, popularity by list rank). Reduction vs hash % [95% CI]; hit ratio of B2h.\n\n| alpha | suffix | hash ns | " +
                " | ".join(LABEL[e] for e in eng) + " | B2h hit |\n|" + "---|" * (len(eng) + 4) + "\n")
        for k in sorted(P, key=lambda k: (float(k[0]), k[1])):
            f.write(f"| {k[0]} | {k[1]} | {P[k]['hash']['abs']:.0f} | " + " | ".join(fmt_red(P[k].get(e)) for e in eng) +
                    f" | {P[k]['hot_b2h']['hit']:.3f} |\n")
    SUMMARY["domain"] = {f"{k[0]}|{k[1]}": {e: {kk: v[kk] for kk in ("red", "lo", "hi", "hit", "abs")} for e, v in P[k].items()}
                         for k in P}
    fig, ax = plt.subplots(figsize=(W1, 2.2))
    alphas = sorted({k[0] for k in P}, key=float)
    for i, e in enumerate(["hot_b2h", "pbc", "kim", "full"]):
        errorbar(ax, [float(a) for a in alphas], [P[(a, "ts")][e] for a in alphas], e, offset=(i - 1.5) * 0.012)
    ax.axhline(0, color="#52514e", linewidth=0.9, linestyle="--")
    ax.set_xticks([float(a) for a in alphas])
    ax.set_xlabel("Zipf α over domain rank")
    ax.set_ylabel("Latency reduction vs hash (%)")
    ax.set_title("1M domain FIB, timestamp suffix")
    ax.legend(frameon=False, fontsize=6)
    fig.savefig(FIG / "fig_domain.pdf")
    plt.close(fig)


def an_suffix():
    d = load("suffix")
    if not d:
        return
    P = paired(d, ["n", "alpha"])
    with open(TAB / "suffix.md", "w") as f:
        f.write("Repeated object names (/object/<i mod 257>). Reduction vs hash % [95% CI]; (hit ratio).\n\n| FIB | alpha | hash ns | B2h | PBC | Full-name |\n|---|---|---|---|---|---|\n")
        for k in sorted(P, key=lambda k: (int(k[0]), float(k[1]))):
            f.write(f"| {int(k[0]):,} | {k[1]} | {P[k]['hash']['abs']:.0f} | " + " | ".join(
                f"{fmt_red(P[k][e])} ({P[k][e]['hit']:.2f})" for e in ["hot_b2h", "pbc", "full"]) + " |\n")
    SUMMARY["suffix"] = {f"{k[0]}|{k[1]}": {e: {kk: v[kk] for kk in ("red", "lo", "hi", "hit")} for e, v in P[k].items()}
                         for k in P}


def an_tail():
    d = load("tail")
    if not d:
        return
    g = collections.defaultdict(lambda: collections.defaultdict(list))
    for r in d:
        g[(r["alpha"], r["engine"])]["p50"].append(r["p50_ns"])
        g[(r["alpha"], r["engine"])]["p90"].append(r["p90_ns"])
        g[(r["alpha"], r["engine"])]["p99"].append(r["p99_ns"])
        g[(r["alpha"], r["engine"])]["p999"].append(r["p999_ns"])
        g[(r["alpha"], r["engine"])]["mean"].append(r["mean_ns"])
        g[(r["alpha"], r["engine"])]["hitc"].append(r["first_ns"])
        g[(r["alpha"], r["engine"])]["missc"].append(r["last_ns"])
    eng = ["hash", "hash2s", "full", "kim", "pbc", "hot_b2", "hot_b2h"]
    with open(TAB / "tail.md", "w") as f:
        f.write("Per-lookup latency (TSC, 2M FIB, C=1024; medians over seeds x runs). Hit/miss costs over the last 100K requests.\n\n"
                "| alpha | engine | mean | p50 | p90 | p99 | p99.9 | hit cost | miss cost |\n|---|---|---|---|---|---|---|---|---|\n")
        for a in sorted({k[0] for k in g}, key=float):
            for e in eng:
                v = g[(a, e)]
                if not v:
                    continue
                m = lambda x: st.median(v[x])
                hc = f"{m('hitc'):.0f}" if e not in ("hash", "hash2s") else "--"
                f.write(f"| {a} | {LABEL.get(e, e)} | {m('mean'):.0f} | {m('p50'):.0f} | {m('p90'):.0f} | {m('p99'):.0f} | "
                        f"{m('p999'):.0f} | {hc} | {m('missc'):.0f} |\n")
    SUMMARY["tail"] = {f"{k[0]}|{k[1]}": {x: st.median(v[x]) for x in v} for k, v in g.items()}
    fig, ax = plt.subplots(figsize=(W1 * 1.35, 2.3))
    a = "1.2"
    xs = np.arange(len(eng))
    for j, (q, alpha_) in enumerate([("p50", 1.0), ("p99", 0.45)]):
        vals = [st.median(g[(a, e)][q]) for e in eng]
        ax.bar(xs + (j - 0.5) * 0.38, vals, width=0.36, color=[STYLE[e][0] for e in eng], alpha=alpha_,
               label=q.upper())
    ax.set_xticks(xs)
    short = {"hash": "Hash", "hash2s": "Two-stage", "full": "Full-name", "kim": "Kim", "pbc": "PBC",
             "hot_b2": "HP B2", "hot_b2h": "HP B2h"}
    ax.set_xticklabels([short[e] for e in eng], fontsize=6.5)
    ax.set_ylabel("Per-lookup latency (ns)")
    ax.set_title("2M FIB, α = 1.2: P50 (solid) and P99 (light)")
    ax.grid(axis="x", visible=False)
    fig.savefig(FIG / "fig_tail.pdf")
    plt.close(fig)


def an_mem():
    d = load("mem")
    if not d:
        return
    base = {}
    for r in d:
        if r["engine"] == "hash":
            base[(r["n"], r["C"])] = r["state_bytes"]
    with open(TAB / "memory.md", "w") as f:
        f.write("Allocator-counted memory (bytes live after the pass). Engine state = engine bytes minus the hash-view run "
                "(harness buffers); the shared FIB and guard index are listed separately.\n\n| FIB | C | engine | engine state (KiB) |\n|---|---|---|---|\n")
        fib = {}
        for r in d:
            if r["engine"] == "hash":
                fib[r["n"]] = (r["fib_trie_bytes"], r["fib_hash_bytes"], r["fib_2s_bytes"], r["guard_index_bytes"])
                continue
            f.write(f"| {int(r['n']):,} | {int(r['C'])} | {LABEL.get(r['engine'], r['engine'])} | "
                    f"{(r['state_bytes'] - base[(r['n'], r['C'])]) / 1024:.0f} |\n")
        f.write("\n| FIB | trie (MiB) | hash FIB (MiB) | two-stage hash (MiB) | guard index (MiB) |\n|---|---|---|---|---|\n")
        for n, (t, h, s2, gi) in sorted(fib.items(), key=lambda kv: int(kv[0])):
            f.write(f"| {int(n):,} | {t / 2 ** 20:.0f} | {h / 2 ** 20:.0f} | {s2 / 2 ** 20:.0f} | {gi / 2 ** 20:.0f} |\n")
    SUMMARY["mem"] = [{k: r[k] for k in ("n", "C", "engine", "state_bytes", "fib_trie_bytes", "fib_hash_bytes",
                                         "fib_2s_bytes", "guard_index_bytes")} for r in d]


def an_updates():
    eng = ["hot_b2h", "hot_b2h:L=0", "pbc", "kim", "full"]
    P = bars("updates", "U", eng, "fig_updates.pdf", "500K FIB, α = 1.2: one FIB change every U lookups",
             keyorder=["1000", "10000", "100000"])
    if not P:
        return
    with open(TAB / "updates.md", "w") as f:
        f.write("FIB churn (500K FIB, alpha=1.2). Reduction vs hash % [95% CI]; (hit ratio).\n\n| U | hash ns | " +
                " | ".join(LABEL.get(e, e) for e in eng) + " |\n|" + "---|" * (len(eng) + 2) + "\n")
        for k in ["1000", "10000", "100000"]:
            if (k,) in P:
                f.write(f"| {k} | {P[(k,)]['hash']['abs']:.0f} | " + " | ".join(
                    f"{fmt_red(P[(k,)][e])} ({P[(k,)][e]['hit']:.2f})" for e in eng) + " |\n")
    SUMMARY["updates"] = {k[0]: {e: {kk: v[kk] for kk in ("red", "lo", "hi", "hit")} for e, v in P[k].items()} for k in P}


def an_followup():
    out = {}
    for exp, keys in [("followup_domain", ["alpha", "suffix"]), ("followup_synth", ["n", "alpha"]),
                      ("followup_dynamics", ["workload"])]:
        d = load(exp)
        if not d:
            continue
        for base in ["hash", "hash2s"]:
            if base == "hash2s" and exp == "followup_dynamics":
                continue
            P = paired(d, keys, base=base)
            engs = sorted({r["engine"] for r in d} - {base})
            with open(TAB / f"{exp}_vs_{base}.md", "w") as f:
                f.write(f"{exp}: reduction vs {LABEL.get(base, base)} % [95% CI]; (hit ratio).\n\n| config | {LABEL.get(base, base)} ns | " +
                        " | ".join(LABEL.get(e, e) for e in engs) + " |\n|" + "---|" * (len(engs) + 2) + "\n")
                for k in sorted(P):
                    f.write(f"| {' '.join(k)} | {P[k][base]['abs']:.0f} | " + " | ".join(
                        f"{fmt_red(P[k][e])} ({P[k][e]['hit']:.2f})" for e in engs if e in P[k]) + " |\n")
            out[f"{exp}|{base}"] = {"|".join(k): {e: {kk: v[kk] for kk in ("red", "lo", "hi", "hit", "abs")} for e, v in P[k].items()}
                                    for k in P}
    SUMMARY["followup"] = out


def an_stageA():
    res = {}
    # A1: lower bound
    d = load("A1_nest")
    if d:
        P = paired(d, ["nest", "alpha"])
        Plb = paired(d, ["nest", "alpha"], base="hpx:LB=0")
        lbrate = collections.defaultdict(list)
        for r in d:
            if r["engine"] == "hpx":
                lbrate[(r["nest"], r["alpha"])].append(r["false_misses"] / r["queries"])
        with open(TAB / "stageA_lowerbound.md", "w") as f:
            f.write("A1 lower-bound fallback (500K FIB, timestamp suffix, C=1024; 3 seeds x 4 runs). Reduction vs hash % [95% CI]; "
                    "last column: HP-A vs HP-A without LB.\n\n| nest | alpha | hash ns | HP B2h (stage 0) | HP-A no LB | HP-A | LB resolves/lookup | HP-A vs no-LB |\n|---|---|---|---|---|---|---|---|\n")
            for k in sorted(P, key=lambda k: (float(k[0]), float(k[1]))):
                f.write(f"| {k[0]} | {k[1]} | {P[k]['hash']['abs']:.0f} | {fmt_red(P[k]['hot_b2h'])} | {fmt_red(P[k]['hpx:LB=0'])} | "
                        f"{fmt_red(P[k]['hpx'])} | {st.mean(lbrate[k]):.4f} | {fmt_red(Plb[k]['hpx'])} |\n")
        res["A1"] = {f"{k[0]}|{k[1]}": {"vs_hash": {e: P[k][e]["red"] for e in P[k]}, "lb_gain": Plb[k]["hpx"]["red"],
                                        "lb_lo": Plb[k]["hpx"]["lo"], "lb_hi": Plb[k]["hpx"]["hi"]} for k in P}
        fig, ax = plt.subplots(figsize=(W1, 2.2))
        for a, ls in [("1.0", ":"), ("1.2", "-")]:
            ks = sorted([k for k in Plb if k[1] == a], key=lambda k: float(k[0]))
            x = [float(k[0]) for k in ks]
            y = [Plb[k]["hpx"]["red"] for k in ks]
            lo = [Plb[k]["hpx"]["lo"] for k in ks]
            hi = [Plb[k]["hpx"]["hi"] for k in ks]
            ax.plot(x, y, color="#2a78d6", marker="o", linestyle=ls, label=f"α = {a}")
            ax.vlines(x, lo, hi, color="#2a78d6", linewidth=1.0)
        ax.axhline(0, color="#52514e", linewidth=0.9, linestyle="--")
        ax.set_xlabel("Fraction of FIB entries that are aggregates")
        ax.set_ylabel("Latency reduction from LB (%)")
        ax.set_title("Lower-bound fallback vs full fallback (500K FIB)")
        ax.legend(frameon=False)
        fig.savefig(FIG / "fig_stageA_lowerbound.pdf")
        plt.close(fig)
    # A2: capacity
    d = load("A2_capacity")
    if d:
        P = paired(d, ["alpha", "C"])
        eng = ["hot_b2h", "hpx", "hpx:L=16384", "hpx:L=0/S=16", "pbc"]
        with open(TAB / "stageA_capacity.md", "w") as f:
            f.write("A2 capacity (2M FIB, nest 0.3, timestamp suffix). Reduction vs hash % [95% CI] (hit ratio).\n\n| alpha | C | hash ns | " +
                    " | ".join(LABEL[e] for e in eng) + " |\n|" + "---|" * (len(eng) + 3) + "\n")
            for k in sorted(P, key=lambda k: (float(k[0]), int(k[1]))):
                f.write(f"| {k[0]} | {k[1]} | {P[k]['hash']['abs']:.0f} | " + " | ".join(
                    f"{fmt_red(P[k][e])} ({P[k][e]['hit']:.3f})" for e in eng) + " |\n")
        res["A2"] = {f"{k[0]}|{k[1]}": {e: {"red": P[k][e]["red"], "hit": P[k][e]["hit"]} for e in P[k]} for k in P}
        fig, axes = plt.subplots(1, 2, figsize=(W2, 2.2))
        cs = sorted({int(k[1]) for k in P})
        cols = {"hot_b2h": "#8a8984", "hpx": "#2a78d6", "hpx:L=0/S=16": "#6250d6", "pbc": "#eb6834"}
        for a, ls in [("1.0", ":"), ("1.2", "-")]:
            for e, c in cols.items():
                axes[0].plot(cs, [P[(a, str(x))][e]["red"] for x in cs], color=c, linestyle=ls, marker="o",
                             label=f"{LABEL[e]}, α={a}")
                axes[1].plot(cs, [P[(a, str(x))][e]["hit"] for x in cs], color=c, linestyle=ls, marker="o")
        for ax in axes:
            ax.set_xscale("log", base=2); ax.set_xticks(cs); ax.set_xticklabels([str(c) for c in cs])
            ax.set_xlabel("Cache capacity C")
        axes[0].axhline(0, color="#52514e", linewidth=0.9, linestyle="--")
        axes[0].set_ylabel("Latency reduction vs hash (%)")
        axes[1].set_ylabel("Hit ratio")
        axes[0].legend(frameon=False, fontsize=5.5, ncol=2)
        fig.savefig(FIG / "fig_stageA_capacity.pdf")
        plt.close(fig)
    # A3: tracking mode
    d = load("A3_tracking")
    if d:
        P = paired(d, ["workload", "n", "alpha"])
        modes = ["hpx:L=16384", "hpx", "hpx:L=0/S=4", "hpx:L=0/S=8", "hpx:L=0/S=16", "hpx:L=0/S=32"]
        order = sorted(P, key=lambda k: (k[0] != "stationary", k[0], int(k[1]), float(k[2])))
        with open(TAB / "stageA_tracking.md", "w") as f:
            f.write("A3 tracking mode (C=1024, nest 0.3, timestamp suffix). Reduction vs hash % [95% CI] (hit ratio).\n\n| workload | FIB | alpha | hash ns | " +
                    " | ".join(LABEL[e] for e in modes) + " |\n|" + "---|" * (len(modes) + 4) + "\n")
            for k in order:
                f.write(f"| {k[0]} | {int(k[1]):,} | {k[2]} | {P[k]['hash']['abs']:.0f} | " + " | ".join(
                    f"{fmt_red(P[k][e])} ({P[k][e]['hit']:.2f})" for e in modes) + " |\n")
            f.write("\n| mode | mean over configs | worst config | mean over alpha=1.2 configs |\n|---|---|---|---|\n")
            summ = {}
            for e in modes:
                v = [P[k][e]["red"] for k in order]
                v12 = [P[k][e]["red"] for k in order if k[2] == "1.2"]
                summ[e] = (st.mean(v), min(v), st.mean(v12))
                f.write(f"| {LABEL[e]} | {st.mean(v):+.1f} | {min(v):+.1f} | {st.mean(v12):+.1f} |\n")
        res["A3"] = {"|".join(k): {e: {"red": P[k][e]["red"], "lo": P[k][e]["lo"], "hi": P[k][e]["hi"], "hit": P[k][e]["hit"]}
                                    for e in P[k]} for k in P}
        res["A3_summary"] = summ
        fig, ax = plt.subplots(figsize=(W2, 2.3))
        wdt = 0.8 / len(modes)
        cmap = ["#86b6ef", "#2a78d6", "#1baf7a", "#eda100", "#6250d6", "#e34948"]
        for i, e in enumerate(modes):
            x = np.arange(len(order)) + (i - (len(modes) - 1) / 2) * wdt
            y = [P[k][e]["red"] for k in order]
            lo = [P[k][e]["lo"] for k in order]
            hi = [P[k][e]["hi"] for k in order]
            ax.bar(x, y, width=wdt * 0.92, color=cmap[i], label=LABEL[e])
            ax.errorbar(x, y, yerr=[np.array(y) - np.array(lo), np.array(hi) - np.array(y)], fmt="none",
                        ecolor="#52514e", elinewidth=0.7, capsize=1.2)
        ax.axhline(0, color="#52514e", linewidth=0.9)
        ax.set_xticks(range(len(order)))
        ax.set_xticklabels([f"{k[0]}\n{int(k[1]) // 10**6}M, α={k[2]}" for k in order], fontsize=6)
        ax.set_ylabel("Latency reduction vs hash (%)")
        ax.grid(axis="x", visible=False)
        ax.legend(frameon=False, fontsize=6, ncol=3)
        fig.savefig(FIG / "fig_stageA_tracking.pdf")
        plt.close(fig)
    SUMMARY["stageA"] = res


def an_stageB():
    res = {}
    d = load("B_main")
    if d:
        P = paired(d, ["n", "alpha"])
        byp = collections.defaultdict(list)
        for r in d:
            if r["engine"] == "hpb":
                byp[(r["n"], r["alpha"])].append(r["bypass_lookups"] / r["queries"])
        eng = ["hash#aa", "hash2s", "hot_b2h", "hpx:L=0/S=16", "hpb:AB=0", "hpb"]
        rows_ = sorted(P, key=lambda k: (int(k[0]), float(k[1])))
        with open(TAB / "stageB_main.md", "w") as f:
            f.write("Stage B main grid (interleaved measurement, 10 seeds x 3 runs, nest 0.3, timestamp suffix, C=1024). "
                    "Reduction vs hash FIB % [95% CI over seeds]; 'A/A' = second copy of the hash FIB (noise floor); "
                    "bypass = share of lookups HP-B served on the bypass path.\n\n| FIB | alpha | hash ns | " +
                    " | ".join(LABEL[e] for e in eng) + " | HP-B seeds > 0 | bypass share | HP-B hit |\n|" + "---|" * (len(eng) + 6) + "\n")
            for k in rows_:
                f.write(f"| {int(k[0]):,} | {k[1]} | {P[k]['hash']['abs']:.0f} | " + " | ".join(fmt_red(P[k][e]) for e in eng) +
                        f" | {P[k]['hpb']['pos']}/{P[k]['hpb']['n']} | {st.mean(byp[k]):.2f} | {P[k]['hpb']['hit']:.3f} |\n")
            worst = min(P[k]["hpb"]["red"] for k in rows_)
            worst_lo = min(P[k]["hpb"]["lo"] for k in rows_)
            aa = max(max(abs(P[k]["hash#aa"]["lo"]), abs(P[k]["hash#aa"]["hi"])) for k in rows_)
            f.write(f"\nGate: worst HP-B cell {worst:+.1f}% (lowest CI bound {worst_lo:+.1f}%); A/A |CI| bound {aa:.1f}%.\n")
        # best-of baseline (hash or two-stage) per config
        best = {}
        for k in rows_:
            b = "hash2s" if P[k]["hash2s"]["red"] > 0 else "hash"
            best[k] = b
        Pb2 = paired(d, ["n", "alpha"], base="hash2s")
        with open(TAB / "stageB_vs_best_fib.md", "w") as f:
            f.write("HP-B against the faster of the two FIBs in each configuration (reduction %, [95% CI]).\n\n| FIB | alpha | best FIB | HP-B vs best |\n|---|---|---|---|\n")
            for k in rows_:
                st_ = P[k]["hpb"] if best[k] == "hash" else Pb2[k]["hpb"]
                f.write(f"| {int(k[0]):,} | {k[1]} | {LABEL[best[k]]} | {fmt_red(st_)} |\n")
        res["main"] = {f"{k[0]}|{k[1]}": {e: {kk: P[k][e][kk] for kk in ("red", "lo", "hi", "pos", "n", "abs", "hit")}
                                          for e in P[k]} | {"bypass_share": st.mean(byp[k])} for k in rows_}
        alphas = sorted({k[1] for k in rows_}, key=float)
        ns = sorted({int(k[0]) for k in rows_})
        fig, axes = plt.subplots(1, len(alphas), figsize=(W2, 2.3), sharey=True)
        show = [("hpb", "#2a78d6", "o", "HP-B"), ("hpb:AB=0", "#6250d6", "X", "HP-B without bypass"),
                ("hot_b2h", "#8a8984", "s", "B2h (stage 0)"), ("hash2s", "#e87ba4", "v", "Two-stage hash")]
        for ax, a in zip(np.atleast_1d(axes), alphas):
            lo = [P[(str(n), a)]["hash#aa"]["lo"] for n in ns]
            hi = [P[(str(n), a)]["hash#aa"]["hi"] for n in ns]
            ax.fill_between(ns, lo, hi, color="#c3c2b7", alpha=0.6, linewidth=0, label="A/A noise band")
            for i, (e, c, mk, lab) in enumerate(show):
                x = np.array(ns, dtype=float) * (1 + (i - 1.5) * 0.04)
                y = [P[(str(n), a)][e]["red"] for n in ns]
                l = [P[(str(n), a)][e]["lo"] for n in ns]
                h = [P[(str(n), a)][e]["hi"] for n in ns]
                ax.plot(x, y, color=c, marker=mk, label=lab)
                ax.vlines(x, l, h, color=c, linewidth=1.0)
            ax.axhline(0, color="#52514e", linewidth=0.9, linestyle="--")
            ax.set_xscale("log"); ax.set_xticks(ns)
            ax.set_xticklabels([f"{n // 1000}K" if n < 10 ** 6 else f"{n // 10 ** 6}M" for n in ns])
            ax.set_title(f"Zipf α = {a}"); ax.set_xlabel("FIB size (prefixes)")
        np.atleast_1d(axes)[0].set_ylabel("Latency reduction vs hash FIB (%)")
        np.atleast_1d(axes)[-1].legend(frameon=False, fontsize=6, loc="lower right")
        fig.savefig(FIG / "fig_stageB_main.pdf")
        plt.close(fig)
    for exp, keys, eng, title in [
            ("B_capacity", ["alpha", "C"], ["hash#aa", "pbc", "hpx:L=0/S=16", "hpb:U=0/TF=0", "hpb:AB=0", "hpb"], "capacity (2M FIB)"),
            ("B_dynamics", ["workload", "alpha"], ["hash#aa", "hpx:L=0/S=16", "hpb:AB=0", "hpb"], "workload dynamics (2M FIB)"),
            ("B_domain", ["alpha"], ["hash#aa", "hash2s", "hpb", "hpb_2s"], "domain FIB (1M)"),
            ("B_paper", ["tag"], ["hash#aa", "hot_b2h", "hpb"], "manuscript dataset (50K, no nesting)"),
            ("B_modecheck", ["tag"], ["hash#aa", "hpb"], "sequential vs interleaved measurement (2M, alpha 1.2)")]:
        d = load(exp)
        if not d:
            continue
        P = paired(d, keys)
        byp = collections.defaultdict(list)
        for r in d:
            if r["engine"] == "hpb":
                byp[tuple(r[k] for k in keys)].append(r["bypass_lookups"] / r["queries"])
        with open(TAB / f"stageB_{exp[2:]}.md", "w") as f:
            f.write(f"Stage B, {title}. Reduction vs hash FIB % [95% CI] (hit ratio).\n\n| {' | '.join(keys)} | hash ns | " +
                    " | ".join(LABEL.get(e, e) for e in eng) + " | HP-B bypass share |\n|" + "---|" * (len(keys) + len(eng) + 2) + "\n")
            for k in sorted(P):
                f.write(f"| {' | '.join(k)} | {P[k]['hash']['abs']:.0f} | " + " | ".join(
                    f"{fmt_red(P[k][e])} ({P[k][e]['hit']:.2f})" if e in P[k] else "--" for e in eng) +
                    f" | {st.mean(byp[k]) if byp[k] else float('nan'):.2f} |\n")
        res[exp] = {"|".join(k): {e: {kk: P[k][e][kk] for kk in ("red", "lo", "hi", "hit")} for e in P[k]} for k in P}
    SUMMARY["stageB"] = res


def an_stageC():
    d = load("C_updates")
    if not d:
        return
    P = paired(d, ["n", "alpha", "U", "target"])
    Pinc = paired(d, ["n", "alpha", "U", "target"], base="hpx:L=0/S=16")
    work = collections.defaultdict(lambda: collections.defaultdict(list))
    for r in d:
        if r["engine"] == "hpx:L=0/S=16/INC=1" and r["U"] != "0":
            nupd = r["queries"] / float(r["U"])
            work[(r["n"], r["alpha"], r["U"], r["target"])]["ref"].append(r["guard_refreshes"] / nupd)
            work[(r["n"], r["alpha"], r["U"], r["target"])]["inv"].append(r["invalidations"] / nupd)
    keyf = lambda k: (int(k[0]), float(k[1]), k[3], int(k[2]))
    with open(TAB / "stageC_updates.md", "w") as f:
        f.write("Stage C: FIB churn (interleaved, 3 seeds x 3 runs, nest 0.3, C=1024). One update (alternately remove / re-insert) "
                "every U lookups; target 'hot' = the prefix the current request resolves to. Reduction vs hash FIB % [95% CI] (hit ratio); "
                "last columns: incremental vs clear-on-update, and maintenance work per update.\n\n"
                "| FIB | alpha | target | U | hash ns | A/A | HP-A clear | HP-A incremental | PBC (clear) | incremental vs clear | guard refreshes/update | evictions/update |\n"
                "|---|---|---|---|---|---|---|---|---|---|---|---|\n")
        for k in sorted(P, key=keyf):
            w = work.get(k)
            f.write(f"| {int(k[0]):,} | {k[1]} | {k[3]} | {k[2]} | {P[k]['hash']['abs']:.0f} | {fmt_red(P[k]['hash#aa'])} | "
                    f"{fmt_red(P[k]['hpx:L=0/S=16'])} ({P[k]['hpx:L=0/S=16']['hit']:.2f}) | "
                    f"{fmt_red(P[k]['hpx:L=0/S=16/INC=1'])} ({P[k]['hpx:L=0/S=16/INC=1']['hit']:.2f}) | "
                    + (f"{fmt_red(P[k]['pbc'])} ({P[k]['pbc']['hit']:.2f})" if 'pbc' in P[k] else "--") + " | "
                    f"{fmt_red(Pinc[k]['hpx:L=0/S=16/INC=1'])} | "
                    + (f"{st.mean(w['ref']):.3f} | {st.mean(w['inv']):.3f}" if w else "-- | --") + " |\n")
    SUMMARY["stageC"] = {"|".join(k): {e: {kk: P[k][e][kk] for kk in ("red", "lo", "hi", "hit")} for e in P[k]} |
                         {"inc_vs_clear": Pinc[k]["hpx:L=0/S=16/INC=1"]["red"]} for k in P}
    fig, axes = plt.subplots(1, 2, figsize=(W2, 2.3), sharey=True)
    for ax, tgt in zip(axes, ["uniform", "hot"]):
        for a, ls in [("1.0", ":"), ("1.2", "-")]:
            ks = sorted([k for k in P if k[0] == "500000" and k[1] == a and k[3] == tgt], key=lambda k: int(k[2]))
            if not ks:
                continue
            x = [int(k[2]) for k in ks]
            for e, c, mk in [("hpx:L=0/S=16", "#8a8984", "s"), ("hpx:L=0/S=16/INC=1", "#2a78d6", "o")]:
                y = [P[k][e]["red"] for k in ks]
                ax.plot(x, y, color=c, marker=mk, linestyle=ls, label=f"{'incremental' if 'INC' in e else 'clear on update'}, α={a}")
                ax.vlines(x, [P[k][e]["lo"] for k in ks], [P[k][e]["hi"] for k in ks], color=c, linewidth=0.9)
            ref = [k for k in P if k[0] == "500000" and k[1] == a and k[2] == "0"]
            if ref:
                ax.axhline(P[ref[0]]["hpx:L=0/S=16"]["red"], color="#2a78d6", linewidth=0.8, linestyle=ls, alpha=0.5)
        ax.axhline(0, color="#52514e", linewidth=0.9, linestyle="--")
        ax.set_xscale("log")
        ax.set_xlabel("Lookups between FIB updates (U)")
        ax.set_title(f"500K FIB, {tgt} updates")
    axes[0].set_ylabel("Latency reduction vs hash FIB (%)")
    axes[1].legend(frameon=False, fontsize=6, loc="upper left", bbox_to_anchor=(1.01, 1.0))
    fig.savefig(FIG / "fig_stageC_updates.pdf")
    plt.close(fig)


def check_errors():
    bad = 0
    total = 0
    for p in RES.glob("*/*_runs.csv"):
        for r in rows(p):
            total += 1
            bad += int(r["errors"]) != 0
    SUMMARY["validation"] = {"engine_runs": total, "runs_with_errors": bad}


if __name__ == "__main__":
    for fn in [an_paper, an_main, an_timeline, an_nest, an_capacity, an_dynamics, an_cutoff, an_domain, an_suffix,
               an_tail, an_mem, an_updates, an_followup, an_stageA, an_stageB, an_stageC, check_errors]:
        try:
            fn()
        except Exception as ex:  # keep going; report
            print("analysis step failed:", fn.__name__, repr(ex), file=sys.stderr)
            raise
    (TAB / "summary.json").write_text(json.dumps(SUMMARY, indent=1, default=float))
    print("ok", SUMMARY.get("validation"))
