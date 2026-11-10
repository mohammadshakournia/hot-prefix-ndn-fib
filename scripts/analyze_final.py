#!/usr/bin/env python3
"""Tables, figures and a summary for the final runs (scripts/run_final.py).

Usage: python scripts/analyze_final.py --results results/final/<host>
Writes <results>/tables/*.md, <results>/figures/*.pdf and <results>/SUMMARY.md.
Statistics: within-run paired reduction vs the hash FIB, median over runs per seed, mean over seeds with a
95% percentile bootstrap over seeds; 'hash#aa' is an identical copy of the hash FIB (A/A noise floor).
"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", required=True)
    args = ap.parse_args()
    res = Path(args.results).resolve()
    os.environ["FIBV3_RESULTS"] = str(res)
    os.environ["FIBV3_TABLES"] = str(res / "tables")
    os.environ["FIBV3_FIGURES"] = str(res / "figures")
    sys.path.insert(0, str(ROOT / "tools"))
    import numpy as np
    import matplotlib.pyplot as plt
    import analyze as A  # uses the environment variables above

    A.LABEL.update({"hp": "Hot-prefix (final)", "hp_inc": "Hot-prefix, incremental", "hp_2s": "Hot-prefix over two-stage",
                    "hot_b2": "Hot-prefix B2 (earlier design)", "hot_all": "Prefix cache, admit-all"})
    A.STYLE["hp"] = ("#2a78d6", "o", "Hot-prefix (final)")
    A.STYLE["hp_inc"] = ("#2a78d6", "o", "Hot-prefix, incremental")
    A.STYLE["hp_2s"] = ("#6250d6", "X", "Hot-prefix over two-stage")
    A.STYLE["hot_b2"] = ("#8a8984", "s", "Hot-prefix B2 (earlier design)")
    summary = {}

    def generic(exp, base="hash", keys=None, title="", outname=None):
        outname = outname or exp
        d = A.load(exp)
        if not d:
            return None, None
        tagkeys = keys or [k for k in A.parse_tag(d[0]["tag"])]
        P = A.paired(d, tagkeys, base=base)
        engines = []
        for r in d:
            if r["engine"] not in engines and r["engine"] != base:
                engines.append(r["engine"])
        rows_ = sorted(P, key=lambda k: tuple(float(x) if x.replace(".", "", 1).isdigit() else x for x in k))
        with open(A.TAB / f"{outname}.md", "w") as f:
            f.write(f"{title}\n\nReduction of mean latency vs {A.LABEL.get(base, base)} % [95% CI over seeds] (hit ratio). "
                    "'A/A' = identical copy of the baseline (noise floor).\n\n")
            f.write("| " + " | ".join(tagkeys) + f" | {base} ns | " + " | ".join(
                "A/A" if e == "hash#aa" else A.LABEL.get(e, e) for e in engines) + " |\n")
            f.write("|" + "---|" * (len(tagkeys) + 1 + len(engines)) + "\n")
            for k in rows_:
                cells = []
                for e in engines:
                    s = P[k].get(e)
                    cells.append("--" if not s else (A.fmt_red(s) + (f" ({s['hit']:.2f})" if s["hit"] > 0 else "")))
                f.write("| " + " | ".join(k) + f" | {P[k][base]['abs']:.0f} | " + " | ".join(cells) + " |\n")
        summary[outname] = {"|".join(k): {e: {x: P[k][e][x] for x in ("red", "lo", "hi", "hit", "pos", "n")} for e in P[k]}
                        for k in P}
        return P, rows_

    # F1 main grid + figure
    P, ks = generic("F1_main", keys=["n", "alpha"], title="F1: FIB size x popularity skew (synthetic, nest 0.3, timestamp suffix, C=1024)")
    if P:
        ns = sorted({int(k[0]) for k in ks})
        alphas = sorted({k[1] for k in ks}, key=float)
        fig, axes = plt.subplots(1, len(alphas), figsize=(A.W2, 2.4), sharey=True)
        show = ["hp", "hot_b2", "hash2s", "pbc", "kim", "full"]
        for ax, a in zip(np.atleast_1d(axes), alphas):
            ax.fill_between(ns, [P[(str(n), a)]["hash#aa"]["lo"] for n in ns], [P[(str(n), a)]["hash#aa"]["hi"] for n in ns],
                            color="#c3c2b7", alpha=0.6, linewidth=0, label="A/A noise band")
            for i, e in enumerate(show):
                if e in P[(str(ns[0]), a)]:
                    A.errorbar(ax, ns, [P[(str(n), a)][e] for n in ns], e, offset=(i - 2.5) * 0.03, logx=True)
            ax.axhline(0, color="#52514e", linewidth=0.9, linestyle="--")
            ax.set_xscale("log"); ax.set_xticks(ns)
            ax.set_xticklabels([f"{n // 1000}K" if n < 10 ** 6 else f"{n // 10 ** 6}M" for n in ns])
            ax.set_title(f"Zipf α = {a}"); ax.set_xlabel("FIB size (prefixes)")
        np.atleast_1d(axes)[0].set_ylabel("Latency reduction vs hash FIB (%)")
        np.atleast_1d(axes)[-1].legend(frameon=False, fontsize=6, loc="lower right")
        fig.savefig(A.FIG / "F1_main.pdf")
        plt.close(fig)

    generic("F2_modecheck", keys=["n", "mode"], title="F2: sequential vs interleaved measurement (alpha 1.2)")
    generic("F3_nest", keys=["nest", "alpha"], title="F3: nesting sweep (500K FIB)")
    generic("F4_capacity", keys=["alpha", "C"], title="F4: cache capacity (2M FIB)")
    generic("F5_dynamics", keys=["workload", "alpha"], title="F5: non-stationary workloads (2M FIB)")
    P6, k6 = generic("F6_updates", keys=["n", "alpha", "target", "U"], title="F6: FIB churn (flush-on-update vs incremental)")
    if P6:
        fig, axes = plt.subplots(1, 2, figsize=(A.W2, 2.3), sharey=True)
        for ax, tgt in zip(axes, ["uniform", "hot"]):
            for (n, a), ls in [(("500000", "1.2"), "-"), (("500000", "1.0"), ":"), (("2000000", "1.2"), "--")]:
                kk = sorted([k for k in P6 if k[0] == n and k[1] == a and k[2] == tgt], key=lambda k: int(k[3]))
                if not kk:
                    continue
                x = [int(k[3]) for k in kk]
                for e, c in [("hp", "#8a8984"), ("hp_inc", "#2a78d6")]:
                    ax.plot(x, [P6[k][e]["red"] for k in kk], color=c, linestyle=ls, marker="o",
                            label=f"{'incremental' if e == 'hp_inc' else 'flush'}, {int(n) // 1000}K α={a}")
            ax.axhline(0, color="#52514e", linewidth=0.9, linestyle="--")
            ax.set_xscale("log"); ax.set_xlabel("Lookups between FIB updates (U)"); ax.set_title(f"{tgt} updates")
        axes[0].set_ylabel("Latency reduction vs hash FIB (%)")
        axes[1].legend(frameon=False, fontsize=5.5, loc="upper left", bbox_to_anchor=(1.01, 1.0))
        fig.savefig(A.FIG / "F6_updates.pdf")
        plt.close(fig)
    generic("F7_domain", keys=["alpha"], title="F7: domain-name FIB (1M reversed domains, rank popularity)")
    generic("F7_domain", base="hash2s", keys=["alpha"], title="F7 vs the two-stage hash FIB",
            outname="F7_domain_vs_two_stage")
    generic("F8_paper", keys=["tag"], title="F8: first-prototype dataset (50K, no nesting, object suffix, C=512), incl. the prototype engines")

    # F9 tail
    d = A.load("F9_tail")
    if d:
        import collections, statistics as st
        g = collections.defaultdict(lambda: collections.defaultdict(list))
        for r in d:
            for q in ("mean_ns", "p50_ns", "p90_ns", "p99_ns", "p999_ns", "first_ns", "last_ns"):
                g[(r["alpha"], r["engine"])][q].append(r[q])
        with open(A.TAB / "F9_tail.md", "w") as f:
            f.write("F9: per-lookup latency (TSC), 2M FIB, C=1024; medians over seeds x runs. Hit / miss cost over the last 100K requests.\n\n"
                    "| alpha | engine | mean | P50 | P90 | P99 | P99.9 | hit cost | miss cost |\n|---|---|---|---|---|---|---|---|---|\n")
            for (a, e), v in sorted(g.items()):
                m = lambda x: st.median(v[x])
                f.write(f"| {a} | {A.LABEL.get(e, e)} | {m('mean_ns'):.0f} | {m('p50_ns'):.0f} | {m('p90_ns'):.0f} | {m('p99_ns'):.0f} | "
                        f"{m('p999_ns'):.0f} | {m('first_ns'):.0f} | {m('last_ns'):.0f} |\n")
    # F10 memory
    d = A.load("F10_mem")
    if d:
        base = {(r["n"], r["C"]): r["state_bytes"] for r in d if r["engine"] == "hash"}
        with open(A.TAB / "F10_memory.md", "w") as f:
            f.write("F10: allocator-counted memory. Engine state = live bytes after the pass minus the hash-FIB run (harness buffers).\n\n"
                    "| FIB | C | engine | engine state (KiB) |\n|---|---|---|---|\n")
            for r in d:
                if r["engine"] != "hash":
                    f.write(f"| {int(r['n']):,} | {int(r['C'])} | {A.LABEL.get(r['engine'], r['engine'])} | "
                            f"{(r['state_bytes'] - base[(r['n'], r['C'])]) / 1024:.0f} |\n")
            f.write("\n| FIB | trie (MiB) | hash FIB (MiB) | two-stage (MiB) | guard index (MiB) |\n|---|---|---|---|---|\n")
            seen = set()
            for r in d:
                if r["engine"] == "hash" and r["n"] not in seen:
                    seen.add(r["n"])
                    f.write(f"| {int(r['n']):,} | {r['fib_trie_bytes'] / 2 ** 20:.0f} | {r['fib_hash_bytes'] / 2 ** 20:.0f} | "
                            f"{r['fib_2s_bytes'] / 2 ** 20:.0f} | {r['guard_index_bytes'] / 2 ** 20:.0f} |\n")
    # validation count
    total = bad = 0
    for p in res.glob("*/*_runs.csv"):
        for r in A.rows(p):
            total += 1
            bad += int(r["errors"]) != 0
    summary["validation"] = {"engine_runs": total, "runs_with_errors": bad}
    # F11 cost model
    if (res / "D_profiles.csv").exists():
        subprocess.run([sys.executable, str(ROOT / "tools" / "model.py")], env=os.environ.copy())
    (A.TAB / "summary.json").write_text(json.dumps(summary, indent=1, default=float))
    plat = json.loads((res / "platform.json").read_text()) if (res / "platform.json").exists() else {}
    with open(res / "SUMMARY.md", "w") as f:
        f.write(f"# Final results: {plat.get('host', res.name)}\n\n")
        f.write(f"- OS: {plat.get('os')}\n- CPU: {json.dumps(plat.get('cpu', plat.get('lscpu', ''))[:600] if isinstance(plat.get('cpu', ''), str) else plat.get('cpu'))}\n")
        f.write(f"- Compiler: {plat.get('gxx_version')}\n- Validated engine runs: {total}, runs with LPM errors: {bad}\n\n")
        f.write("Tables: " + ", ".join(sorted(p.name for p in A.TAB.glob('*.md'))) + "\n\n")
        f.write("Figures: " + ", ".join(sorted(p.name for p in A.FIG.glob('*.pdf'))) + "\n")
        if "F1_main" in summary:
            f.write("\n## F1 headline (Hot-prefix final vs hash FIB, % [95% CI])\n\n| FIB | alpha | Hot-prefix | A/A |\n|---|---|---|---|\n")
            for k, v in sorted(summary["F1_main"].items(), key=lambda kv: (int(kv[0].split('|')[0]), float(kv[0].split('|')[1]))):
                n, a = k.split("|")
                hp, aa = v.get("hp"), v.get("hash#aa")
                if hp and aa:
                    f.write(f"| {int(n):,} | {a} | {hp['red']:+.1f} [{hp['lo']:+.1f}, {hp['hi']:+.1f}] | "
                            f"{aa['red']:+.1f} [{aa['lo']:+.1f}, {aa['hi']:+.1f}] |\n")
    print(f"analysis written to {res}/tables, {res}/figures, {res}/SUMMARY.md; validated runs {total}, errors {bad}")


if __name__ == "__main__":
    main()
