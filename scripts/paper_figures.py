#!/usr/bin/env python3
"""Publication figures for the Hot-prefix paper, built from one final result set (scripts/run_final.py).

Usage: python scripts/paper_figures.py --results results/final/<host> [--out <dir>]
Writes vector PDFs (and 300-dpi PNG previews) to <out> (default <results>/paper_figures).
Every data figure uses the paired statistics of tools/analyze.py: within-run reduction vs the hash FIB,
median over runs per seed, mean over seeds with a 95% percentile bootstrap. Missing experiments are skipped.
"""
import argparse
import collections
import os
import shutil
import statistics as st
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

ap = argparse.ArgumentParser()
ap.add_argument("--results", required=True)
ap.add_argument("--out", default=None)
ap.add_argument("--compare", default=None, help="second result set (another platform) for the replication figure")
ap.add_argument("--labels", default="Platform A|Platform B")
ARGS = ap.parse_args()
RES = Path(ARGS.results).resolve()
OUT = Path(ARGS.out).resolve() if ARGS.out else RES / "paper_figures"
OUT.mkdir(parents=True, exist_ok=True)
os.environ["FIBV3_RESULTS"] = str(RES)
os.environ.setdefault("FIBV3_TABLES", str(RES / "tables"))
os.environ.setdefault("FIBV3_FIGURES", str(RES / "figures"))
sys.path.insert(0, str(ROOT / "tools"))

import numpy as np  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.patches import FancyBboxPatch  # noqa: E402
import analyze as A  # noqa: E402

INK, INK2, MUTED, GRID, BAND = "#1f1e1b", "#52514e", "#8a8984", "#e4e3df", "#d9d8d2"
plt.rcParams.update({"font.family": "DejaVu Sans", "axes.labelcolor": INK, "text.color": INK,
                     "xtick.color": INK2, "ytick.color": INK2, "legend.frameon": False})

# Fixed engine identity: colour, marker, label. Order = legend order.
E = {
    "hp":      ("#2a78d6", "o", "Hot-prefix (this work)"),
    "hp_inc":  ("#2a78d6", "o", "Hot-prefix, incremental"),
    "hp_2s":   ("#6250d6", "X", "Hot-prefix over two-stage"),
    "hot_b2":  ("#008300", "P", "Hot-prefix B2 (earlier design)"),
    "hot_all": ("#e34948", "*", "Prefix cache, admit-all"),
    "hash2s":  ("#e87ba4", "v", "Two-stage hash FIB"),
    "pbc":     ("#eb6834", "s", "PBC"),
    "kim":     ("#1baf7a", "^", "Kim (128-bit)"),
    "kim8":    ("#1baf7a", "<", "Kim (8-bit)"),
    "full":    ("#eda100", "D", "Full-name cache"),
    "hash":    (INK2, "", "Hash FIB"),
    "trie":    (MUTED, "h", "Component trie"),
}
SAVED = []


def kfmt(n):
    n = int(float(n))
    return f"{n // 1000}K" if n < 10 ** 6 else f"{n // 10 ** 6}M"


def save(fig, name):
    fig.savefig(OUT / f"{name}.pdf")
    fig.savefig(OUT / f"{name}.png", dpi=300)
    plt.close(fig)
    SAVED.append(name)


def err(ax, x, stats, e, off=0.0, logx=False, ls="-", label=None):
    c, mk, lab = E[e]
    x = np.asarray(x, float)
    x = x * (1 + off) if logx else x + off
    y = np.array([s["red"] for s in stats]); lo = np.array([s["lo"] for s in stats]); hi = np.array([s["hi"] for s in stats])
    ax.plot(x, y, color=c, marker=mk, linestyle=ls, label=label or lab, zorder=3, linewidth=1.4, markersize=4.5,
            markeredgecolor="white", markeredgewidth=0.6)
    ok = ~np.isnan(lo)
    ax.vlines(x[ok], lo[ok], hi[ok], color=c, linewidth=0.9, zorder=2)


def zero(ax):
    ax.axhline(0, color=INK2, linewidth=0.8, linestyle="--", zorder=1)


def aa_band(ax, x, stats):
    ax.fill_between(x, [s["lo"] for s in stats], [s["hi"] for s in stats], color=BAND, linewidth=0, zorder=0,
                    label="A/A noise (95% CI)")


def bottom_legend(fig, handles, labels, ncol, h_in=0.2, fontsize=6.5):
    """Legend in its own strip below the axes (never overlapping tick or axis labels)."""
    fig.tight_layout()
    H = fig.get_size_inches()[1]
    rows = -(-len(labels) // ncol)
    strip = (h_in + 0.13 * (rows - 1)) / H
    fig.subplots_adjust(bottom=fig.subplotpars.bottom + strip)
    fig.legend(handles, labels, loc="lower center", ncol=ncol, bbox_to_anchor=(0.5, 0.0), fontsize=fontsize,
               handlelength=1.8, columnspacing=1.2)


def bars(ax, groups, engines, val, errs=None, width=0.8, labels=True):
    """Grouped bars with a 2px-equivalent gap; groups on x, engines inside a group."""
    k = len(engines)
    w = width / k
    for j, e in enumerate(engines):
        c = E[e][0]
        xs = np.arange(len(groups)) + (j - (k - 1) / 2) * w
        ys = [val(g, e) for g in groups]
        ax.bar(xs, ys, w * 0.88, color=c, label=E[e][2] if labels else None, zorder=2)
        if errs:
            lo = [errs(g, e)[0] for g in groups]; hi = [errs(g, e)[1] for g in groups]
            ax.vlines(xs, lo, hi, color=INK, linewidth=0.8, zorder=3)
    ax.set_xticks(np.arange(len(groups)))


# ------------------------------------------------------------------ 1. architecture / lookup path
def _fig_arch():
    fig, ax = plt.subplots(figsize=(A.W2, 2.25))
    ax.set_xlim(0, 100); ax.set_ylim(0, 31); ax.axis("off")

    def box(x, y, w, h, text, fc="#f3f2ee", ec=MUTED, bold=False, size=8.4):
        ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.25,rounding_size=1.2", fc=fc, ec=ec, lw=0.9))
        ax.text(x + w / 2, y + h / 2, text, ha="center", va="center", fontsize=size, color=INK,
                fontweight="bold" if bold else "normal", linespacing=1.25)

    def arrow(x0, y0, x1, y1, text="", color=INK2, dy=1.2, ha="center"):
        ax.annotate("", xy=(x1, y1), xytext=(x0, y0), arrowprops=dict(arrowstyle="-|>", color=color, lw=0.9,
                                                                     shrinkA=0, shrinkB=0, mutation_scale=8))
        if text:
            ax.text((x0 + x1) / 2, (y0 + y1) / 2 + dy, text, fontsize=7.2, color=INK2, ha=ha, va="bottom")

    blue = "#dbe8f8"
    box(0.5, 13, 8.5, 6, "Interest\nname $q$")
    box(12, 12, 18, 8, "Front end\none-pass parse,\nprefix hashes $H(1..m)$", fc=blue, ec="#2a78d6", size=7.6)
    box(34, 12, 23, 8, "Hot-prefix cache\nflat index, probed from the\nlongest resident depth", fc=blue, ec="#2a78d6",
        size=7.6)
    box(61, 12, 16, 8, "Guard test\nnext component\nof $q$ not in $G(p)$?", fc=blue, ec="#2a78d6", size=7.6)
    box(83, 21, 16.5, 7, "Hit: face of $p$\nexact LPM (Thm. 1)", fc="#dff0e5", ec="#008300", size=7.6)
    box(83, 3, 16.5, 7, "Miss: FIB lookup\n(hash or two-stage)", size=7.6)
    box(34, 1, 23, 7, "Admission: frequency sketch\n(sampled 1/16), CLOCK", size=7.6)
    box(34, 23.5, 23, 7, "FIB update (Thm. 2): evict,\nrefresh face, recompute\nancestor guards", fc="#fbeee6",
        ec="#eb6834", size=7.2)
    arrow(9, 16, 12, 16)
    arrow(30, 16, 34, 16)
    arrow(57, 16, 61, 16)
    ax.text(59, 16.8, "p", fontsize=7.8, color=INK2, ha="center", va="bottom", style="italic")
    arrow(77, 18, 83, 24.5)
    ax.text(79.4, 22.2, "pass", fontsize=7.2, color=INK2, ha="right")
    arrow(77, 14, 83, 7.5)
    ax.text(79.2, 9.4, "fail, or no\nresident $p$", fontsize=7.2, color=INK2, ha="right", va="center")
    ax.annotate("", xy=(57, 4.5), xytext=(83, 4.5), arrowprops=dict(arrowstyle="-|>", color=INK2, lw=0.8,
                                                                     mutation_scale=7, linestyle=":"))
    ax.text(70, 2.6, "candidate prefix", fontsize=7.2, color=INK2, ha="center", va="center")
    arrow(45.5, 8, 45.5, 12)
    ax.text(46.5, 10, "admit", fontsize=7.2, color=INK2, ha="left", va="center")
    ax.annotate("", xy=(45.5, 20), xytext=(45.5, 23.5), arrowprops=dict(arrowstyle="-|>", color="#eb6834", lw=0.9,
                                                                     mutation_scale=8))
    save(fig, "fig01_architecture")


def fig_arch():
    """Drawn in STIX, the text font of the Elsevier CAS template, so the diagram matches the body text."""
    with plt.rc_context({"font.family": "STIXGeneral", "mathtext.fontset": "stix"}):
        _fig_arch()


# ------------------------------------------------------------------ 2-3. main grid
def fig_main():
    d = A.load("F1_main")
    if not d:
        return
    P = A.paired(d, ["n", "alpha"])
    ns = sorted({int(float(k[0])) for k in P}); alphas = sorted({k[1] for k in P}, key=float)
    key = lambda n, a: (str(n), a)
    show = ["hp", "hot_b2", "hot_all", "hash2s", "pbc", "kim", "full"]
    fig, axes = plt.subplots(1, len(alphas), figsize=(A.W2, 2.5), sharey=True)
    for ax, a in zip(np.atleast_1d(axes), alphas):
        aa_band(ax, ns, [P[key(n, a)]["hash#aa"] for n in ns])
        for i, e in enumerate(show):
            if e in P[key(ns[0], a)]:
                err(ax, ns, [P[key(n, a)][e] for n in ns], e, off=(i - 3) * 0.035, logx=True)
        zero(ax)
        ax.set_xscale("log"); ax.set_xticks(ns); ax.set_xticklabels([kfmt(n) for n in ns]); ax.minorticks_off()
        ax.set_title(f"Zipf α = {a}"); ax.set_xlabel("FIB size (prefixes)")
    ax0 = np.atleast_1d(axes)[0]
    ax0.set_ylabel("Mean-latency reduction\nvs hash FIB (%)")
    lo = min(-100, ax0.get_ylim()[0]); ax0.set_ylim(max(lo, -160), None)
    h, l = ax0.get_legend_handles_labels()
    bottom_legend(fig, h, l, 4)
    save(fig, "fig02_main_reduction")

    # absolute latency (ns) and hit ratio
    fig, axes = plt.subplots(2, len(alphas), figsize=(A.W2, 3.6), sharex=True)
    for j, a in enumerate(alphas):
        ax = axes[0, j]
        for e in ["hash", "hash2s", "full", "hp"]:
            if e in P[key(ns[0], a)]:
                ax.plot(ns, [P[key(n, a)][e]["abs"] for n in ns], color=E[e][0], marker=E[e][1] or "o", label=E[e][2],
                        linestyle="--" if e == "hash" else "-", markeredgecolor="white", markeredgewidth=0.6)
        ax.set_xscale("log"); ax.set_title(f"Zipf α = {a}"); ax.set_ylim(0, None)
        ax2 = axes[1, j]
        for e in ["hp", "hot_b2", "hot_all"]:
            if e in P[key(ns[0], a)]:
                ax2.plot(ns, [P[key(n, a)][e]["hit"] for n in ns], color=E[e][0], marker=E[e][1], label=E[e][2],
                         markeredgecolor="white", markeredgewidth=0.6)
        ax2.set_ylim(0, 1); ax2.set_xlabel("FIB size (prefixes)")
        ax2.set_xticks(ns); ax2.set_xticklabels([kfmt(n) for n in ns]); ax2.minorticks_off()
    axes[0, 0].set_ylabel("Mean latency (ns/lookup)"); axes[1, 0].set_ylabel("Cache hit ratio")
    h1, l1 = axes[0, 0].get_legend_handles_labels(); h2, l2 = axes[1, 0].get_legend_handles_labels()
    keep = [(h, l) for h, l in zip(h1 + h2, l1 + l2)]
    seen = set(); keep = [(h, l) for h, l in keep if not (l in seen or seen.add(l))]
    bottom_legend(fig, [h for h, _ in keep], [l for _, l in keep], 3, fontsize=6.3)
    save(fig, "fig03_main_latency_hits")


# ------------------------------------------------------------------ 4. FIB probes (deterministic)
def fig_probes():
    d = A.load("F12_probes")
    if not d:
        return
    g = {(int(float(r["n"])), r["alpha"], r["engine"]): r["fib_probes"] / r["queries"] for r in d}
    ns = sorted({k[0] for k in g}); alphas = sorted({k[1] for k in g}, key=float)
    engines = [e for e in ["hash", "hash2s", "kim", "pbc", "hp"] if any(k[2] == e for k in g)]
    fig, axes = plt.subplots(1, len(alphas), figsize=(A.W2, 2.2), sharey=True)
    for ax, a in zip(np.atleast_1d(axes), alphas):
        bars(ax, ns, engines, lambda n, e: g.get((n, a, e), np.nan), labels=(a == alphas[0]))
        ax.set_xticklabels([kfmt(n) for n in ns]); ax.set_title(f"Zipf α = {a}"); ax.set_xlabel("FIB size (prefixes)")
        ax.grid(axis="x", visible=False)
    np.atleast_1d(axes)[0].set_ylabel("FIB probes per lookup")
    bottom_legend(fig, *np.atleast_1d(axes)[0].get_legend_handles_labels(), len(engines))
    save(fig, "fig04_fib_probes")


# ------------------------------------------------------------------ 5. timeline (warm-up and steady state)
def fig_timeline():
    import csv
    p = RES / "F1_main" / "n2000000_a1.2_s1_windows.csv"
    if not p.exists():
        return
    w = collections.defaultdict(lambda: collections.defaultdict(list))
    with open(p) as f:
        for r in csv.DictReader(f):
            w[r["engine"]][int(r["window"])].append(float(r["latency_ns"]))
    fig, ax = plt.subplots(figsize=(A.W1, 2.1))
    for e in ["hash", "hash2s", "hp"]:
        if e not in w:
            continue
        xs = sorted(w[e]); y = np.array([st.median(w[e][x]) for x in xs])
        k = 5
        ys = np.convolve(y, np.ones(k) / k, mode="valid")
        x = (np.array(xs[k - 1:]) * 5000) / 1000
        ax.plot(x, ys, color=E[e][0], linestyle="--" if e == "hash" else "-", label=E[e][2], linewidth=1.3)
    ax.set_xlabel("Requests processed (thousands)"); ax.set_ylabel("Latency (ns/lookup)")
    ax.set_ylim(0, None); ax.legend(fontsize=6.3, loc="lower left")
    ax.set_title("2M FIB, Zipf α = 1.2 (5K-lookup windows, 5-window mean)", fontsize=7)
    save(fig, "fig05_timeline")


# ------------------------------------------------------------------ 6. nesting
def fig_nest():
    d = A.load("F3_nest")
    if not d:
        return
    P = A.paired(d, ["nest", "alpha"])
    nests = sorted({k[0] for k in P}, key=float); alphas = sorted({k[1] for k in P}, key=float)
    fig, axes = plt.subplots(1, len(alphas), figsize=(A.W1, 2.2), sharey=True)
    for ax, a in zip(np.atleast_1d(axes), alphas):
        x = [100 * float(n) for n in nests]
        aa_band(ax, x, [P[(n, a)]["hash#aa"] for n in nests])
        for i, e in enumerate(["hp", "pbc", "kim", "kim8"]):
            if e in P[(nests[0], a)]:
                err(ax, x, [P[(n, a)][e] for n in nests], e, off=(i - 1.5) * 0.8, ls="--" if e == "kim8" else "-")
        zero(ax); ax.set_title(f"α = {a}"); ax.set_xlabel("Aggregate prefixes (%)")
    np.atleast_1d(axes)[0].set_ylabel("Reduction vs hash FIB (%)")
    h, l = np.atleast_1d(axes)[0].get_legend_handles_labels()
    bottom_legend(fig, h, l, 3, fontsize=6)
    save(fig, "fig06_nesting")


# ------------------------------------------------------------------ 7. capacity
def fig_capacity():
    d = A.load("F4_capacity")
    if not d:
        return
    P = A.paired(d, ["alpha", "C"])
    alphas = sorted({k[0] for k in P}, key=float); Cs = sorted({k[1] for k in P}, key=float)
    fig, axes = plt.subplots(1, 2, figsize=(A.W1, 2.2))
    x = [int(float(c)) for c in Cs]
    styles = {alphas[0]: ":", alphas[-1]: "-"}
    for a in alphas:
        for e in ["hp", "pbc", "kim"]:
            if e in P[(a, Cs[0])]:
                err(axes[0], x, [P[(a, c)][e] for c in Cs], e, logx=True, ls=styles[a],
                    label=f"{E[e][2].split(' (')[0]}, α={a}")
        axes[1].plot(x, [P[(a, c)]["hp"]["hit"] for c in Cs], color=E["hp"][0], marker="o", linestyle=styles[a],
                     label=f"α = {a}", markeredgecolor="white", markeredgewidth=0.6)
    for ax in axes:
        ax.set_xscale("log"); ax.set_xticks(x); ax.set_xticklabels([f"{c // 1024}K" if c >= 1024 else str(c) for c in x])
        ax.minorticks_off(); ax.set_xlabel("Cache capacity C (entries)")
    zero(axes[0]); axes[0].set_ylabel("Reduction vs hash FIB (%)")
    axes[1].set_ylabel("Hot-prefix hit ratio"); axes[1].set_ylim(0, 1); axes[1].legend(fontsize=6, loc="lower right")
    h, l = axes[0].get_legend_handles_labels()
    bottom_legend(fig, h, l, 2, fontsize=6)
    save(fig, "fig07_capacity")


# ------------------------------------------------------------------ 8. non-stationary traffic
def fig_dynamics():
    d = A.load("F5_dynamics")
    if not d:
        return
    P = A.paired(d, ["workload", "alpha"])
    wl = [w for w in ["stationary", "phaseshift", "scan", "churn"] if any(k[0] == w for k in P)]
    alphas = sorted({k[1] for k in P}, key=float)
    names = {"stationary": "stationary", "phaseshift": "phase shift", "scan": "scan", "churn": "churn"}
    fig, axes = plt.subplots(1, len(alphas), figsize=(A.W1, 2.2), sharey=True)
    for ax, a in zip(np.atleast_1d(axes), alphas):
        bars(ax, wl, ["hot_b2", "hp"], lambda w, e: P[(w, a)][e]["red"],
             errs=lambda w, e: (P[(w, a)][e]["lo"], P[(w, a)][e]["hi"]), labels=(a == alphas[0]))
        lo = [P[(w, a)]["hash#aa"]["lo"] for w in wl]; hi = [P[(w, a)]["hash#aa"]["hi"] for w in wl]
        ax.axhspan(min(lo), max(hi), color=BAND, zorder=0, linewidth=0)
        ax.set_xticklabels([names[w] for w in wl], rotation=30, ha="right"); zero(ax); ax.set_title(f"α = {a}")
        ax.grid(axis="x", visible=False)
    np.atleast_1d(axes)[0].set_ylabel("Reduction vs hash FIB (%)")
    bottom_legend(fig, *np.atleast_1d(axes)[0].get_legend_handles_labels(), 2, fontsize=6)
    save(fig, "fig08_dynamics")


# ------------------------------------------------------------------ 9. FIB updates
def fig_updates():
    d = A.load("F6_updates")
    if not d:
        return
    P = A.paired(d, ["n", "alpha", "target", "U"])
    cfgs = [c for c in [("500000", "1.0"), ("500000", "1.2"), ("2000000", "1.2")] if any(k[:2] == c for k in P)]
    fig, axes = plt.subplots(1, 2, figsize=(A.W2, 2.3), sharey=True)
    lss = {("500000", "1.0"): ":", ("500000", "1.2"): "-", ("2000000", "1.2"): "--"}
    for ax, tgt in zip(axes, ["uniform", "hot"]):
        for c in cfgs:
            kk = sorted([k for k in P if k[:2] == c and k[2] == tgt], key=lambda k: int(k[3]))
            if not kk:
                continue
            x = [int(k[3]) for k in kk]
            base = [k for k in P if k[:2] == c and k[3] == "0"]
            for e, col in [("hp", MUTED), ("hp_inc", E["hp"][0])]:
                ax.plot(x, [P[k][e]["red"] for k in kk], color=col, linestyle=lss[c], marker="o", markersize=3.8,
                        markeredgecolor="white", markeredgewidth=0.5,
                        label=f"{'incremental (Thm. 2)' if e == 'hp_inc' else 'flush on update'}, {kfmt(c[0])} α={c[1]}")
            if base:
                ax.axhline(P[base[0]]["hp"]["red"], color=E["hp"][0], linestyle=lss[c], linewidth=0.7, alpha=0.45,
                           label=f"no updates, {kfmt(c[0])} α={c[1]}")
        zero(ax); ax.set_xscale("log"); ax.set_xlabel("Lookups between FIB updates")
        ax.set_title("updates to random prefixes" if tgt == "uniform" else "updates to the current request's prefix")
    axes[0].set_ylabel("Reduction vs hash FIB (%)")
    h, l = axes[0].get_legend_handles_labels()
    order = sorted(range(len(l)), key=lambda i: (l[i].split(",")[0] != "incremental (Thm. 2)", l[i].split(",")[0] != "flush on update", i))
    bottom_legend(fig, [h[i] for i in order], [l[i] for i in order], 3, fontsize=6)
    save(fig, "fig09_updates")


# ------------------------------------------------------------------ 10. domain-name FIB
def fig_domain():
    d = A.load("F7_domain")
    if not d:
        return
    alphas = sorted({r["alpha"] for r in d}, key=float)
    fig, axes = plt.subplots(1, 2, figsize=(A.W2 * 0.75, 2.6))
    for ax, base, eng in [(axes[0], "hash", ["hp", "hash2s", "pbc", "kim", "full"]), (axes[1], "hash2s", ["hp_2s", "hp"])]:
        P = A.paired(d, ["alpha"], base=base)
        eng = [e for e in eng if e in P[(alphas[0],)]]
        bars(ax, alphas, eng, lambda a, e: P[(a,)][e]["red"], errs=lambda a, e: (P[(a,)][e]["lo"], P[(a,)][e]["hi"]))
        aa = [P[(a,)].get("hash#aa") for a in alphas] if base == "hash" else None
        if aa and all(aa):
            ax.axhspan(min(s["lo"] for s in aa), max(s["hi"] for s in aa), color=BAND, zorder=0, linewidth=0)
        ax.set_xticklabels([f"α = {a}" for a in alphas]); zero(ax); ax.grid(axis="x", visible=False)
        ax.set_ylabel("Reduction vs hash FIB (%)" if base == "hash" else "Reduction vs two-stage (%)")
        lab = {"hp": "Hot-prefix over hash FIB"} if base == "hash2s" else {}
        hh, ll = ax.get_legend_handles_labels()
        ax.legend(hh, [lab.get(e, x) for e, x in zip(eng, ll)], fontsize=5.8, loc="upper center",
                  bbox_to_anchor=(0.5, -0.13), ncol=2)
    axes[0].set_title("baseline: hash FIB"); axes[1].set_title("baseline: two-stage hash FIB")
    fig.tight_layout(w_pad=3.0)
    save(fig, "fig10_domain")


# ------------------------------------------------------------------ 11. tail latency
def fig_tail():
    d = A.load("F9_tail")
    if not d:
        return
    g = collections.defaultdict(lambda: collections.defaultdict(list))
    for r in d:
        for q in ("p50_ns", "p90_ns", "p99_ns", "p999_ns", "mean_ns"):
            g[(r["alpha"], r["engine"])][q].append(r[q])
    alphas = sorted({k[0] for k in g}, key=float)
    engines = [e for e in ["hash", "hash2s", "pbc", "hp"] if any(k[1] == e for k in g)]
    qs = [("p50_ns", "P50"), ("p90_ns", "P90"), ("p99_ns", "P99"), ("p999_ns", "P99.9")]
    fig, axes = plt.subplots(1, len(alphas), figsize=(A.W1, 2.2), sharey=True)
    for ax, a in zip(np.atleast_1d(axes), alphas):
        bars(ax, [q for q, _ in qs], engines, lambda q, e: st.median(g[(a, e)][q]), labels=(a == alphas[0]))
        ax.set_yscale("log"); ax.set_xticklabels([l for _, l in qs]); ax.set_title(f"α = {a}")
        ax.grid(axis="x", visible=False)
    np.atleast_1d(axes)[0].set_ylabel("Per-lookup latency (ns)")
    bottom_legend(fig, *np.atleast_1d(axes)[0].get_legend_handles_labels(), 2, fontsize=6)
    save(fig, "fig11_tail")


# ------------------------------------------------------------------ 12. measurement mode (A/A)
def fig_modecheck():
    d = A.load("F2_modecheck")
    if not d:
        return
    P = A.paired(d, ["n", "mode"])
    ns = sorted({k[0] for k in P}, key=float)
    fig, ax = plt.subplots(figsize=(A.W1, 2.0))
    groups = [(n, m) for n in ns for m in ["timeline", "interleave"]]
    xs = np.arange(len(groups))
    for j, (e, lab, col) in enumerate([("hp", "Hot-prefix", E["hp"][0]), ("hash#aa", "A/A copy of hash FIB", MUTED)]):
        x = xs + (j - 0.5) * 0.36
        ax.bar(x, [P[gk][e]["red"] for gk in groups], 0.32, color=col, label=lab, zorder=2)
        ax.vlines(x, [P[gk][e]["lo"] for gk in groups], [P[gk][e]["hi"] for gk in groups], color=INK, linewidth=0.8, zorder=3)
    ax.set_xticks(xs); ax.set_xticklabels([f"{kfmt(n)}\n{'sequential' if m == 'timeline' else 'interleaved'}" for n, m in groups])
    zero(ax); ax.set_ylabel("Reduction vs hash FIB (%)"); ax.grid(axis="x", visible=False)
    bottom_legend(fig, *ax.get_legend_handles_labels(), 2, fontsize=6)
    ax.set_title("Zipf α = 1.2", fontsize=7)
    save(fig, "fig12_measurement_mode")


# ------------------------------------------------------------------ 13. first-prototype dataset incl. v2
def fig_paperdata():
    d = A.load("F8_paper")
    if not d:
        return
    P = A.paired(d, ["tag"])
    s = P[next(iter(P))]
    order = [("v2_trie", "trie (v2)"), ("v2_hash", "hash (v2)"), ("v2_hot_b1", "B1 (v2)"), ("v2_hot_b2", "B2 (v2)"),
             ("trie", "trie"), ("hash", "hash"), ("kim", "Kim"), ("pbc", "PBC"), ("hot_b1", "B1"), ("hot_b2", "B2"),
             ("hp", "Hot-prefix")]
    order = [(e, l) for e, l in order if e in s]
    fig, ax = plt.subplots(figsize=(A.W1, 2.1))
    cols = [MUTED if e.startswith("v2_") else (E["hp"][0] if e == "hp" else "#9db9de") for e, _ in order]
    ax.bar(range(len(order)), [s[e]["abs"] for e, _ in order], 0.75, color=cols, zorder=2)
    ax.set_yscale("log"); ax.set_xticks(range(len(order)))
    from matplotlib.ticker import FuncFormatter, LogLocator
    top = max(s[e]["abs"] for e, _ in order) * 1.9
    ax.set_ylim(min(s[e]["abs"] for e, _ in order) * 0.7, top)
    ax.yaxis.set_major_locator(LogLocator(subs=(1, 2, 5)))
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:g}")); ax.yaxis.set_minor_formatter(FuncFormatter(lambda v, _: "")); ax.set_xticklabels([l for _, l in order], rotation=45, ha="right")
    ax.set_ylabel("Mean latency (ns/lookup)"); ax.grid(axis="x", visible=False)
    ax.text(1.5, top * 0.78, "original implementation", ha="center", fontsize=6, color=INK2)
    ax.text(7.5, top * 0.78, "same front end (this work)", ha="center", fontsize=6, color=INK2)
    ax.axvline(3.5, color=MUTED, linewidth=0.7)
    save(fig, "fig13_prototype_dataset")


# ------------------------------------------------------------------ 14-15. cost model
def fig_model():
    if not (RES / "D_profiles.csv").exists():
        return
    fdir = Path(os.environ["FIBV3_FIGURES"])
    subprocess.run([sys.executable, str(ROOT / "tools" / "model.py")], env=os.environ.copy(), check=False,
                   stdout=subprocess.DEVNULL)
    for src, dst in [("fig_stageD_pred_vs_measured.pdf", "fig14_model_pred_vs_measured"),
                     ("fig_stageD_regime_map.pdf", "fig15_model_regime_map")]:
        if (fdir / src).exists():
            shutil.copy(fdir / src, OUT / f"{dst}.pdf")
            subprocess.run(["pdftoppm", "-png", "-r", "300", "-singlefile", str(OUT / f"{dst}.pdf"), str(OUT / dst)],
                           check=False)
            SAVED.append(dst)



# ------------------------------------------------------------------ 16. replication on a second platform
def fig_platforms():
    if not ARGS.compare:
        return
    import os as _os
    sets = []
    for path in [RES, Path(ARGS.compare).resolve()]:
        A.RES = path
        d = A.load("F1_main")
        sets.append(A.paired(d, ["n", "alpha"]))
    A.RES = RES
    labs = ARGS.labels.split("|")
    sty = [("#2a78d6", "o", "-"), ("#eb6834", "s", "--")]
    P0 = sets[0]
    ns = sorted({int(float(k[0])) for k in P0}); alphas = sorted({k[1] for k in P0}, key=float)
    fig, axes = plt.subplots(1, len(alphas), figsize=(A.W2, 2.3), sharey=True)
    for ax, a in zip(np.atleast_1d(axes), alphas):
        for i, (P, lab, (c, mk, ls)) in enumerate(zip(sets, labs, sty)):
            x = np.array(ns, float) * (1 + (i - 0.5) * 0.06)
            st_ = [P[(str(n), a)]["hp"] for n in ns]
            ax.plot(x, [v["red"] for v in st_], color=c, marker=mk, linestyle=ls, label=lab, markersize=4.5,
                    markeredgecolor="white", markeredgewidth=0.6, linewidth=1.4)
            ax.vlines(x, [v["lo"] for v in st_], [v["hi"] for v in st_], color=c, linewidth=0.9)
        zero(ax); ax.set_xscale("log"); ax.set_xticks(ns); ax.set_xticklabels([kfmt(n) for n in ns]); ax.minorticks_off()
        ax.set_title(f"Zipf α = {a}"); ax.set_xlabel("FIB size (prefixes)")
    np.atleast_1d(axes)[0].set_ylabel("Hot-prefix reduction\nvs hash FIB (%)")
    bottom_legend(fig, *np.atleast_1d(axes)[0].get_legend_handles_labels(), 2)
    save(fig, "fig16_platforms")

for f in [fig_arch, fig_main, fig_probes, fig_timeline, fig_nest, fig_capacity, fig_dynamics, fig_updates, fig_domain,
          fig_tail, fig_modecheck, fig_paperdata, fig_model, fig_platforms]:
    try:
        f()
    except Exception as ex:  # noqa: BLE001
        print(f"{f.__name__}: skipped ({type(ex).__name__}: {ex})")
print("figures:", ", ".join(SAVED), "->", OUT)
