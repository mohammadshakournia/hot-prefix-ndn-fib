#!/usr/bin/env python3
"""Stage D: cost model for Hot-prefix.

  dT = T_hash - T_cache = h (F_hot - c_hit) - (1 - h) (delta0 + eps F_cold)

Inputs per configuration that an operator can obtain without running the cache:
  h            hit ratio from a deterministic replay of the cache logic over a trace
  F_hot,F_cold hash-FIB cost of the requests that would hit / miss (per-lookup timing of the
               existing FIB, split by the replayed hit set)
  T_hash       hash-FIB mean cost
Machine constants (calibrated once): c_hit, delta0, eps.

Validation:
  (a) calibrate the constants on two configurations, predict every other one;
  (b) leave-one-configuration-out.
Targets: R_seq = measured reduction in the same sequential profiling runs, and R_il = the
interleaved measurement of the Stage B grid (independent runs).
"""
import collections
import csv
import json
import statistics as st
import sys
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from analyze import load, paired, W1, W2, RES, TAB, FIG  # noqa: E402


def parse_tag(t):
    parts = t.split(";")
    d = {"family": parts[0]}
    for p in parts[1:]:
        k, v = p.split("=")
        d[k] = v
    return d


def load_profiles():
    rows = list(csv.DictReader(open(RES / "D_profiles.csv")))
    g = collections.defaultdict(list)
    for r in rows:
        t = parse_tag(r["tag"])
        key = (t["family"], t["n"], t["alpha"], t["nest"], t["C"])
        g[key].append({k: float(r[k]) for k in ("h", "F_hot", "F_cold", "T_hash", "c_hit", "c_miss", "T_cache")})
    cfg = {}
    for k, v in g.items():
        m = {x: float(np.mean([r[x] for r in v])) for x in v[0]}
        m["R_seq"] = 100 * (m["T_hash"] - m["T_cache"]) / m["T_hash"]
        m["delta"] = m["c_miss"] - m["F_cold"]
        m["samples"] = len(v)
        cfg[k] = m
    return cfg


def _line(x, y):
    x = np.asarray(x, float); y = np.asarray(y, float)
    if len(x) == 1 or np.ptp(x) < 1e-9:
        return float(y.mean()), 0.0
    A = np.vstack([np.ones_like(x), x]).T
    a, b = np.linalg.lstsq(A, y, rcond=None)[0]
    return float(a), float(b)


def fit(cfgs, variant="M1"):
    """M1: c_hit constant.  M2: c_hit = a + b (1 - h).  Both: delta = delta0 + eps F_cold."""
    d0, eps = _line([c["F_cold"] for c in cfgs], [c["delta"] for c in cfgs])
    if variant == "M1":
        return ("M1", float(np.mean([c["c_hit"] for c in cfgs])), 0.0, d0, eps)
    a, b = _line([1 - c["h"] for c in cfgs], [c["c_hit"] for c in cfgs])
    return ("M2", a, b, d0, eps)


def c_hit_of(c, params):
    _, a, b, _, _ = params
    return a + b * (1 - c["h"])


def predict(c, params):
    _, _, _, d0, eps = params
    dT = c["h"] * (c["F_hot"] - c_hit_of(c, params)) - (1 - c["h"]) * (d0 + eps * c["F_cold"])
    return 100 * dT / c["T_hash"]


def breakeven_h(c, params):
    # solve h (F_hot - a - b(1-h)) = (1-h) delta  for h (quadratic in general; bisection)
    _, a, b, d0, eps = params
    delta = d0 + eps * c["F_cold"]
    f = lambda h: h * (c["F_hot"] - a - b * (1 - h)) - (1 - h) * delta
    lo, hi = 0.0, 1.0
    if f(hi) <= 0:
        return float("nan")
    for _ in range(60):
        mid = 0.5 * (lo + hi)
        lo, hi = (mid, hi) if f(mid) < 0 else (lo, mid)
    return 0.5 * (lo + hi)


def interleaved_targets():
    """Interleaved reductions of the final design: final runs (F1/F7, engine 'hp') if present,
    otherwise the Stage B runs."""
    out = {}
    for exp, eng in [("F1_main", "hp"), ("B_main", "hpx:L=0/S=16")]:
        d = load(exp)
        if d:
            P = paired(d, ["n", "alpha"])
            for k, v in P.items():
                if eng in v:
                    out[("synth", k[0], k[1], "0.3", "1024")] = v[eng]["red"]
            break
    for exp, eng in [("F7_domain", "hp"), ("B_domain", "hpb")]:
        d = load(exp)
        if d:
            P = paired(d, ["alpha"])
            for k, v in P.items():
                if eng in v:
                    out[("domain", "1000000", k[0], "dom", "1024")] = v[eng]["red"]
            break
    return out


def decision(r, band=3.0):
    return "deploy" if r > band else ("avoid" if r < -band else "neutral")


def main():
    cfg = load_profiles()
    keys = sorted(cfg, key=lambda k: (k[0], int(k[1]), float(k[2]), k[3], int(k[4])))
    il = interleaved_targets()
    main_keys = [k for k in keys if k[4] == "1024" and k[3] in ("0.3", "dom")]
    S = lambda n, a: ("synth", n, a, "0.3", "1024")
    schemes = {
        "M1, 2 configs (50K & 4M, a=1.0)": ("M1", [S("50000", "1.0"), S("4000000", "1.0")]),
        "M2, small FIB only (50K, a=0.8 & 1.2)": ("M2", [S("50000", "0.8"), S("50000", "1.2")]),
        "M2, 3 configs (50K a=0.8, 50K a=1.2, 4M a=1.0)": ("M2", [S("50000", "0.8"), S("50000", "1.2"), S("4000000", "1.0")]),
    }
    results = {}
    for name, (variant, calib) in schemes.items():
        calib = [k for k in calib if k in cfg]
        pa = fit([cfg[k] for k in calib], variant)
        rs = []
        for k in keys:
            c = cfg[k]
            loo = fit([cfg[j] for j in keys if j != k], variant)
            rs.append({"key": k, **c, "pred_cal": predict(c, pa), "pred_loo": predict(c, loo), "R_il": il.get(k),
                       "h_star": breakeven_h(c, pa), "calib": k in calib, "c_hit_pred": c_hit_of(c, pa)})
        results[name] = (pa, calib, rs)

    def errs(field, target, sel):
        e = [abs(r[field] - r[target]) for r in sel if r[target] is not None]
        return (float(np.mean(e)), float(np.max(e)), len(e)) if e else (float("nan"),) * 3

    def acc(field, target, sel):
        ok = [decision(r[field]) == decision(r[target]) for r in sel if r[target] is not None]
        return (sum(ok), len(ok))

    # Final runs: calibration on the smallest FIB alone is ill-conditioned (delta0 and eps cannot be
    # separated when F_cold hardly varies), so the reported scheme uses three configurations that span
    # FIB sizes; the development runs used the 50K-only scheme.
    best = "M2, 3 configs (50K a=0.8, 50K a=1.2, 4M a=1.0)" if load("F1_main") else "M2, small FIB only (50K, a=0.8 & 1.2)"
    pa, calib, rows = results[best]
    with open(TAB / "stageD_model.md", "w") as f:
        f.write("Stage D cost model. dT = h (F_hot - c_hit) - (1-h)(delta0 + eps F_cold); M1: c_hit constant, "
                "M2: c_hit = a + b (1-h). Profiles: per-lookup TSC, sequential, hash / cache / hash bracketing, "
                "2-3 seeds x 2 repetitions per configuration.\n\n")
        f.write("| scheme | parameters | held-out MAE vs sequential (pp) | max | decisions | held-out MAE vs interleaved (pp) | max | decisions | LOO MAE seq / il |\n|---|---|---|---|---|---|---|---|---|\n")
        for name, (p_, cal_, rs) in results.items():
            held = [r for r in rs if not r["calib"]]
            ms, mxs, _ = errs("pred_cal", "R_seq", held); as_, ns_ = acc("pred_cal", "R_seq", held)
            mi, mxi, _ = errs("pred_cal", "R_il", held); ai, ni = acc("pred_cal", "R_il", held)
            ls_, _, _ = errs("pred_loo", "R_seq", rs); li, _, _ = errs("pred_loo", "R_il", rs)
            par = f"a={p_[1]:.0f}, b={p_[2]:.0f}, d0={p_[3]:.0f}, eps={p_[4]:.3f}"
            f.write(f"| {name} | {par} | {ms:.1f} | {mxs:.1f} | {as_}/{ns_} | {mi:.1f} | {mxi:.1f} | {ai}/{ni} | {ls_:.1f} / {li:.1f} |\n")
        f.write(f"\nPer-configuration detail for '{best}' (calibration points marked):\n\n")
        f.write("| family | FIB | alpha | nest | C | h | F_hot | F_cold | c_hit meas. / model | delta meas. | break-even h* | "
                "predicted R | measured R seq | measured R interleaved |\n|" + "---|" * 14 + "\n")
        for r in rows:
            k = r["key"]
            ril = f"{r['R_il']:+.1f}" if r["R_il"] is not None else "--"
            mark = " (calib.)" if r["calib"] else ""
            f.write(f"| {k[0]} | {int(k[1]):,} | {k[2]} | {k[3]} | {k[4]} | {r['h']:.3f} | {r['F_hot']:.0f} | {r['F_cold']:.0f} | "
                    f"{r['c_hit']:.0f} / {r['c_hit_pred']:.0f} | {r['delta']:.0f} | {r['h_star']:.2f} | {r['pred_cal']:+.1f}{mark} | "
                    f"{r['R_seq']:+.1f} | {ril} |\n")
        f.write("\nMeasured sequential minus interleaved reduction (same configurations): mean "
                f"{np.mean([r['R_seq'] - r['R_il'] for r in rows if r['R_il'] is not None]):+.1f} pp.\n")
    json.dump({name: {"params": p_, "calib": ["|".join(k) for k in cal_],
                      "rows": [{**{k: v for k, v in r.items() if k != 'key'}, 'key': '|'.join(r['key'])} for r in rs]}
               for name, (p_, cal_, rs) in results.items()}, open(TAB / "stageD_model.json", "w"), indent=1, default=float)

    # Figures: predicted vs measured; regime map
    fig, axes = plt.subplots(1, 2, figsize=(W2, 2.6))
    for ax, target, title in [(axes[0], "R_seq", "sequential measurement"), (axes[1], "R_il", "interleaved measurement")]:
        pts = [r for r in rows if r[target] is not None]
        for r in pts:
            col = "#2a78d6" if r["key"][0] == "synth" else "#eb6834"
            mk = "s" if r["calib"] else ("o" if r["key"][4] == "1024" else "^")
            ax.scatter(r["pred_cal"], r[target], color=col, marker=mk, s=22, zorder=3,
                       edgecolor="white", linewidth=0.6)
        lim = [min(min(r["pred_cal"], r[target]) for r in pts) - 5, max(max(r["pred_cal"], r[target]) for r in pts) + 5]
        ax.plot(lim, lim, color="#52514e", linewidth=0.8, linestyle="--")
        ax.axhline(0, color="#c3c2b7", linewidth=0.8)
        ax.axvline(0, color="#c3c2b7", linewidth=0.8)
        ax.set_xlim(lim); ax.set_ylim(lim)
        ax.set_xlabel("Predicted reduction (%)")
        ax.set_ylabel("Measured reduction (%)")
        ax.set_title(title)
    from matplotlib.lines import Line2D
    axes[1].legend(handles=[Line2D([], [], color="#2a78d6", marker="o", linestyle="", label="synthetic, C=1024"),
                            Line2D([], [], color="#2a78d6", marker="^", linestyle="", label="synthetic, other C / nest"),
                            Line2D([], [], color="#eb6834", marker="o", linestyle="", label="domain FIB"),
                            Line2D([], [], color="#2a78d6", marker="s", linestyle="", label="calibration point")],
                   frameon=False, fontsize=6, loc="upper left")
    fig.savefig(FIG / "fig_stageD_pred_vs_measured.pdf")
    plt.close(fig)

    fig, ax = plt.subplots(figsize=(W1, 2.6))
    fh = np.linspace(min(r["F_hot"] for r in rows) * 0.8, max(r["F_hot"] for r in rows) * 1.1, 200)
    fc_typ = float(np.median([r["F_cold"] for r in rows]))
    hstar = [breakeven_h({"F_hot": x, "F_cold": fc_typ}, pa) for x in fh]
    ax.plot(fh, hstar, color="#52514e", linewidth=1.2)
    for r in rows:
        col = "#2a78d6" if r["R_seq"] > 3 else ("#e34948" if r["R_seq"] < -3 else "#8a8984")
        ax.scatter(r["F_hot"], r["h"], color=col, s=20, edgecolor="white", linewidth=0.6, zorder=3)
    ax.set_xlabel(r"Hash-FIB cost of the requests the cache answers, $F_{\mathrm{hot}}$ (ns)")
    ax.set_ylabel("Hit ratio h")
    ax.set_ylim(0, 1)
    ax.set_title(f"Break-even hit ratio (model M2, F_cold = {fc_typ:.0f} ns)", fontsize=7)
    ax.legend(handles=[Line2D([], [], color="#52514e", label="model break-even h*"),
                       Line2D([], [], color="#2a78d6", marker="o", linestyle="", label="measured gain > 3%"),
                       Line2D([], [], color="#8a8984", marker="o", linestyle="", label="within ±3%"),
                       Line2D([], [], color="#e34948", marker="o", linestyle="", label="measured loss > 3%")],
              frameon=False, fontsize=6, loc="upper right")
    fig.savefig(FIG / "fig_stageD_regime_map.pdf")
    plt.close(fig)
    print("ok", {k: v[0] for k, v in results.items()})


if __name__ == "__main__":
    main()
