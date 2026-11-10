#!/usr/bin/env python3
"""Runs the v3 experiment matrix. Usage: run_all.py [experiment ...]

Every experiment writes results/<exp>/<config>_runs.csv (and _windows.csv for
timeline runs). Datasets are generated on demand under data/.
Engines run in a seeded random order per run; every run is validated.
"""
import itertools
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
DATA, RES = ROOT / "data", ROOT / "results"
GEN = ROOT / "tools" / "gen_v3.py"
DOMAINS = ROOT.parent / "top-1000000-domains" / "top-1000000-domains"
BENCH, BENCH_MEM = ROOT / "bench_v3", ROOT / "bench_v3_mem"
Q = 300_000          # requests per stream
SEEDS = [1, 2, 3, 4, 5]
RUNS = 4

LOG = open(RES / "run_all.log", "a", buffering=1)


def log(*a):
    msg = time.strftime("%H:%M:%S ") + " ".join(str(x) for x in a)
    print(msg, flush=True)
    LOG.write(msg + "\n")


def fib_path(family, n, nest):
    return DATA / (f"{family}_n{n}_nest{nest}.csv" if family == "synth" else f"domain_n{n}.csv")


def q_path(family, n, nest, kind, alpha, rho, suffix, seed, count=Q):
    tag = f"{family}_n{n}_nest{nest}" if family == "synth" else f"domain_n{n}"
    return DATA / f"q_{tag}_{kind}_a{alpha}_r{rho}_{suffix}_c{count}_s{seed}.csv"


def ensure(family, n, nest, streams):
    """streams: list of (kind, alpha, rho, suffix, seed[, count])."""
    fib = fib_path(family, n, nest)
    specs = []
    for st in streams:
        kind, alpha, rho, suffix, seed = st[:5]
        count = st[5] if len(st) > 5 else Q
        p = q_path(family, n, nest, kind, alpha, rho, suffix, seed, count)
        if not p.exists():
            specs.append(f"{kind}:{alpha}:{rho}:{suffix}:{count}:{seed}:{p}")
    if fib.exists() and not specs:
        return
    cmd = [sys.executable, str(GEN), "--family", family, "--fib-size", str(n), "--out-fib", str(fib)]
    if family == "synth":
        cmd += ["--nest", str(nest)]
    else:
        cmd += ["--domains", str(DOMAINS)]
    if specs:
        cmd += ["--queries"] + specs
    log("generate", family, n, nest, len(specs), "streams")
    subprocess.run(cmd, check=True)


def bench(exp, name, fib, qf, engines, seed, cache=1024, runs=RUNS, mode="timeline", extra=(), tag="", mem=False,
          no_trie=True):
    out = RES / exp
    out.mkdir(parents=True, exist_ok=True)
    prefix = out / name
    if Path(str(prefix) + "_runs.csv").exists():
        return
    cmd = [str(BENCH_MEM if mem else BENCH), "--fib", str(fib), "--queries", str(qf), "--engines", ",".join(engines),
           "--runs", str(runs), "--seed", str(seed), "--cache", str(cache), "--mode", mode,
           "--segments", "50000,100000", "--tag", tag or name, "--out", str(prefix)] + list(extra)
    if no_trie:
        cmd.append("--no-trie")
    t = time.time()
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        log("FAILED", name, r.stderr[-2000:])
        raise SystemExit(1)
    log("done", exp, name, f"{time.time() - t:.0f}s")


MAIN_ENGINES = ["hash", "hash2s", "full", "kim", "pbc", "hot_b1", "hot_b2", "hot_b2h", "hot_all"]


def exp_main():
    """FIB size x popularity skew, unique timestamp suffixes, C=1024."""
    for n, alpha in itertools.product([50_000, 500_000, 2_000_000, 4_000_000], [0.8, 1.0, 1.2]):
        ensure("synth", n, 0.3, [("stationary", alpha, 0.5, "ts", s) for s in SEEDS])
        for s in SEEDS:
            qf = q_path("synth", n, 0.3, "stationary", alpha, 0.5, "ts", s)
            bench("main", f"n{n}_a{alpha}_s{s}", fib_path("synth", n, 0.3), qf, MAIN_ENGINES, s,
                  tag=f"n={n};alpha={alpha};C=1024;suffix=ts")


def exp_suffix():
    """Repeated object names versus unique timestamps (full-name cache)."""
    for n, alpha, suffix in itertools.product([500_000, 2_000_000], [1.0, 1.2], ["object"]):
        ensure("synth", n, 0.3, [("stationary", alpha, 0.5, suffix, s) for s in SEEDS])
        for s in SEEDS:
            qf = q_path("synth", n, 0.3, "stationary", alpha, 0.5, suffix, s)
            bench("suffix", f"n{n}_a{alpha}_{suffix}_s{s}", fib_path("synth", n, 0.3), qf,
                  ["hash", "full", "pbc", "hot_b2h"], s, tag=f"n={n};alpha={alpha};suffix={suffix}")


def exp_nest():
    """Nesting sweep: guard rejections and filter false positives."""
    for nest, alpha in itertools.product([0.0, 0.1, 0.3, 0.5], [1.0, 1.2]):
        ensure("synth", 500_000, nest, [("stationary", alpha, 0.5, "ts", s) for s in SEEDS[:3]])
        for s in SEEDS[:3]:
            qf = q_path("synth", 500_000, nest, "stationary", alpha, 0.5, "ts", s)
            bench("nest", f"nest{nest}_a{alpha}_s{s}", fib_path("synth", 500_000, nest), qf,
                  ["hash", "kim", "kim8", "pbc", "hot_b2h"], s, tag=f"nest={nest};alpha={alpha}")


def exp_capacity():
    for alpha, c in itertools.product([1.0, 1.2], [256, 1024, 4096, 16384]):
        ensure("synth", 2_000_000, 0.3, [("stationary", alpha, 0.5, "ts", s) for s in SEEDS[:3]])
        for s in SEEDS[:3]:
            qf = q_path("synth", 2_000_000, 0.3, "stationary", alpha, 0.5, "ts", s)
            bench("capacity", f"a{alpha}_C{c}_s{s}", fib_path("synth", 2_000_000, 0.3), qf,
                  ["hash", "pbc", "kim", "hot_b2h", "hot_b2"], s, cache=c, tag=f"alpha={alpha};C={c}")


def exp_dynamics():
    """Non-stationary workloads, learning cutoff versus continuous learning and decay."""
    kinds = ["stationary", "phaseshift", "scan", "churn"]
    ensure("synth", 2_000_000, 0.3, [(k, 1.0, 0.5, "ts", s) for k in kinds for s in SEEDS[:3]])
    for k in kinds:
        for s in SEEDS[:3]:
            qf = q_path("synth", 2_000_000, 0.3, k, 1.0, 0.5, "ts", s)
            bench("dynamics", f"{k}_s{s}", fib_path("synth", 2_000_000, 0.3), qf,
                  ["hash", "pbc", "hot_b2h", "hot_b2h:L=0", "hot_b2h_decay", "hot_b1", "hot_b1_decay"], s,
                  tag=f"workload={k}")


def exp_cutoff():
    for alpha in [1.0, 1.2]:
        for s in SEEDS[:3]:
            qf = q_path("synth", 2_000_000, 0.3, "stationary", alpha, 0.5, "ts", s)
            ensure("synth", 2_000_000, 0.3, [("stationary", alpha, 0.5, "ts", s)])
            bench("cutoff", f"a{alpha}_s{s}", fib_path("synth", 2_000_000, 0.3), qf,
                  ["hash", "hot_b2h:L=16384", "hot_b2h:L=65536", "hot_b2h:L=262144", "hot_b2h:L=0"], s,
                  tag=f"alpha={alpha}")


def exp_domain():
    """Real ranked domain list (reversed), popularity follows list rank."""
    for alpha, suffix in itertools.product([0.8, 1.0, 1.2], ["ts", "object"]):
        ensure("domain", 1_000_000, 0, [("stationary", alpha, 0.5, suffix, s) for s in SEEDS[:3]])
        for s in SEEDS[:3]:
            qf = q_path("domain", 1_000_000, 0, "stationary", alpha, 0.5, suffix, s)
            bench("domain", f"a{alpha}_{suffix}_s{s}", fib_path("domain", 1_000_000, 0), qf,
                  ["hash", "hash2s", "full", "kim", "pbc", "hot_b2", "hot_b2h"], s,
                  tag=f"alpha={alpha};suffix={suffix}")


def exp_paper():
    """The manuscript's original dataset (50K, no nesting, object suffix), incl. v2 engines."""
    fib = ROOT.parent / "v2ref" / "ds" / "fib_50000.csv"
    for s in [1, 2, 3, 4, 5]:
        qf = DATA / f"q_paper_seed{s}.csv"
        if not qf.exists():
            subprocess.run([sys.executable, str(ROOT.parent / "v2ref" / "generate_ndn_datasets.py"),
                            "--fib-sizes", "50000", "--query-count", "100000", "--workloads", "stationary",
                            "--seeds", str(s), "--output-dir", str(DATA / "paper")], check=True,
                           stdout=subprocess.DEVNULL)
            (DATA / "paper" / f"queries_stationary_fib50000_q100000_seed{s}.csv").rename(qf)
        bench("paper", f"s{s}", fib, qf,
              ["trie", "hash", "kim", "pbc", "hot_b1", "hot_b2", "hot_b2h", "v2_trie", "v2_hash", "v2_hot_b1",
               "v2_hot_b2"], s, cache=512, runs=5, extra=["--segments", "10000,30000"], no_trie=False,
              tag="paper-dataset")


def exp_tail():
    for alpha in [1.0, 1.2]:
        for s in SEEDS[:3]:
            qf = q_path("synth", 2_000_000, 0.3, "stationary", alpha, 0.5, "ts", s)
            bench("tail", f"a{alpha}_s{s}", fib_path("synth", 2_000_000, 0.3), qf,
                  ["hash", "hash2s", "full", "kim", "pbc", "hot_b2", "hot_b2h"], s, mode="tail", runs=3,
                  tag=f"alpha={alpha}")


def exp_mem():
    for n in [500_000, 2_000_000]:
        qf = q_path("synth", n, 0.3, "stationary", 1.0, 0.5, "ts", 1)
        ensure("synth", n, 0.3, [("stationary", 1.0, 0.5, "ts", 1)])
        for c in [1024, 16384]:
            bench("mem", f"n{n}_C{c}", fib_path("synth", n, 0.3), qf,
                  ["hash", "full", "kim", "pbc", "hot_b0", "hot_b1", "hot_b2", "hot_b2h"], 1, cache=c, runs=1,
                  mem=True, no_trie=(n > 500_000), tag=f"n={n};C={c}")


def exp_updates():
    """FIB churn: alternately remove/re-insert a random prefix every U lookups."""
    ensure("synth", 500_000, 0.3, [("stationary", 1.2, 0.5, "ts", s) for s in SEEDS[:3]])
    for u in [1000, 10000, 100000]:
        for s in SEEDS[:3]:
            qf = q_path("synth", 500_000, 0.3, "stationary", 1.2, 0.5, "ts", s)
            bench("updates", f"U{u}_s{s}", fib_path("synth", 500_000, 0.3), qf,
                  ["hash", "full", "kim", "pbc", "hot_b2h", "hot_b2h:L=0"], s,
                  extra=["--update-every", str(u)], no_trie=False, runs=3, tag=f"U={u}")


def exp_dynamics12():
    """Non-stationary workloads in the winning regime (alpha 1.2)."""
    kinds = ["stationary", "phaseshift", "scan", "churn"]
    ensure("synth", 2_000_000, 0.3, [(k, 1.2, 0.5, "ts", s) for k in kinds for s in SEEDS[:3]])
    for k in kinds:
        for s in SEEDS[:3]:
            qf = q_path("synth", 2_000_000, 0.3, k, 1.2, 0.5, "ts", s)
            bench("dynamics12", f"{k}_s{s}", fib_path("synth", 2_000_000, 0.3), qf,
                  ["hash", "pbc", "hot_b2h", "hot_b2h:L=0", "hot_b2h_decay", "hot_b2"], s,
                  tag=f"workload={k}")


def exp_followup():
    """Hot-prefix over the strongest FIB of each dataset, and sampled continuous tracking."""
    for alpha, suffix in itertools.product([1.0, 1.2], ["ts"]):
        for s in SEEDS[:3]:
            qf = q_path("domain", 1_000_000, 0, "stationary", alpha, 0.5, suffix, s)
            bench("followup_domain", f"a{alpha}_{suffix}_s{s}", fib_path("domain", 1_000_000, 0), qf,
                  ["hash", "hash2s", "hot_b2h", "hot_b2h_2s", "hot_b2h:L=0/S=8"], s, tag=f"alpha={alpha};suffix={suffix}")
    for n, alpha in itertools.product([2_000_000, 4_000_000], [1.0, 1.2]):
        for s in SEEDS[:3]:
            qf = q_path("synth", n, 0.3, "stationary", alpha, 0.5, "ts", s)
            bench("followup_synth", f"n{n}_a{alpha}_s{s}", fib_path("synth", n, 0.3), qf,
                  ["hash", "hash2s", "hot_b2h", "hot_b2h_2s", "hot_b2h:L=16384", "hot_b2h:L=0/S=4",
                   "hot_b2h:L=0/S=16"], s, tag=f"n={n};alpha={alpha}")
    kinds = ["stationary", "phaseshift", "scan", "churn"]
    for k in kinds:
        for s in SEEDS[:3]:
            qf = q_path("synth", 2_000_000, 0.3, k, 1.2, 0.5, "ts", s)
            bench("followup_dynamics", f"{k}_s{s}", fib_path("synth", 2_000_000, 0.3), qf,
                  ["hash", "hot_b2h", "hot_b2h:L=0/S=4", "hot_b2h:L=0/S=16"], s, tag=f"workload={k}")


def exp_stageA():
    """Stage A: lower-bound fallback, C-sized sketch, choice of tracking mode."""
    for nest, alpha in itertools.product([0.0, 0.1, 0.3, 0.5], [1.0, 1.2]):
        for s in SEEDS[:3]:
            qf = q_path("synth", 500_000, nest, "stationary", alpha, 0.5, "ts", s)
            bench("A1_nest", f"nest{nest}_a{alpha}_s{s}", fib_path("synth", 500_000, nest), qf,
                  ["hash", "pbc", "hot_b2h", "hpx:LB=0", "hpx"], s, tag=f"nest={nest};alpha={alpha}")
    for alpha, c in itertools.product([1.0, 1.2], [256, 1024, 4096, 16384]):
        for s in SEEDS[:3]:
            qf = q_path("synth", 2_000_000, 0.3, "stationary", alpha, 0.5, "ts", s)
            bench("A2_capacity", f"a{alpha}_C{c}_s{s}", fib_path("synth", 2_000_000, 0.3), qf,
                  ["hash", "pbc", "hot_b2h", "hpx", "hpx:L=16384", "hpx:L=0/S=16"], s, cache=c,
                  tag=f"alpha={alpha};C={c}")
    track = ["hash", "hpx:L=16384", "hpx", "hpx:L=0/S=4", "hpx:L=0/S=8", "hpx:L=0/S=16", "hpx:L=0/S=32"]
    for n, alpha in itertools.product([2_000_000, 4_000_000], [1.0, 1.2]):
        for s in SEEDS[:3]:
            qf = q_path("synth", n, 0.3, "stationary", alpha, 0.5, "ts", s)
            bench("A3_tracking", f"stationary_n{n}_a{alpha}_s{s}", fib_path("synth", n, 0.3), qf, track, s,
                  tag=f"workload=stationary;n={n};alpha={alpha}")
    for k in ["phaseshift", "scan", "churn"]:
        for s in SEEDS[:3]:
            qf = q_path("synth", 2_000_000, 0.3, k, 1.2, 0.5, "ts", s)
            bench("A3_tracking", f"{k}_n2000000_a1.2_s{s}", fib_path("synth", 2_000_000, 0.3), qf, track, s,
                  tag=f"workload={k};n=2000000;alpha=1.2")


IL = ["--window", "5000"]  # interleaved slices


def exp_stageB():
    """Stage B: fill threshold, unit-consistent scores, adaptive bypass. Interleaved measurement,
    A/A control (hash#aa) in every run."""
    seeds10 = list(range(1, 11))
    eng = ["hash", "hash#aa", "hash2s", "hot_b2h", "hpx:L=0/S=16", "hpb", "hpb:AB=0"]
    for n, alpha in itertools.product([50_000, 500_000, 2_000_000, 4_000_000], [0.8, 1.0, 1.2]):
        ensure("synth", n, 0.3, [("stationary", alpha, 0.5, "ts", s) for s in seeds10])
        for s in seeds10:
            qf = q_path("synth", n, 0.3, "stationary", alpha, 0.5, "ts", s)
            bench("B_main", f"n{n}_a{alpha}_s{s}", fib_path("synth", n, 0.3), qf, eng, s, runs=3, mode="interleave",
                  extra=IL, tag=f"n={n};alpha={alpha}")
    for alpha, c in itertools.product([1.0, 1.2], [256, 1024, 4096, 16384]):
        for s in SEEDS[:3]:
            qf = q_path("synth", 2_000_000, 0.3, "stationary", alpha, 0.5, "ts", s)
            bench("B_capacity", f"a{alpha}_C{c}_s{s}", fib_path("synth", 2_000_000, 0.3), qf,
                  ["hash", "hash#aa", "pbc", "hpx:L=0/S=16", "hpb", "hpb:AB=0", "hpb:U=0/TF=0"], s, cache=c, runs=3,
                  mode="interleave", extra=IL, tag=f"alpha={alpha};C={c}")
    for k, alpha in itertools.product(["stationary", "phaseshift", "scan", "churn"], [1.0, 1.2]):
        ensure("synth", 2_000_000, 0.3, [(k, alpha, 0.5, "ts", s) for s in SEEDS[:3]])
        for s in SEEDS[:3]:
            qf = q_path("synth", 2_000_000, 0.3, k, alpha, 0.5, "ts", s)
            bench("B_dynamics", f"{k}_a{alpha}_s{s}", fib_path("synth", 2_000_000, 0.3), qf,
                  ["hash", "hash#aa", "hpx:L=0/S=16", "hpb", "hpb:AB=0"], s, runs=3, mode="interleave", extra=IL,
                  tag=f"workload={k};alpha={alpha}")
    for alpha in [0.8, 1.0, 1.2]:
        for s in SEEDS[:3]:
            qf = q_path("domain", 1_000_000, 0, "stationary", alpha, 0.5, "ts", s)
            bench("B_domain", f"a{alpha}_s{s}", fib_path("domain", 1_000_000, 0), qf,
                  ["hash", "hash#aa", "hash2s", "hpb", "hpb_2s"], s, runs=3, mode="interleave", extra=IL,
                  tag=f"alpha={alpha}")
    fib = ROOT.parent / "v2ref" / "ds" / "fib_50000.csv"
    for s in [1, 2, 3, 4, 5]:
        bench("B_paper", f"s{s}", fib, DATA / f"q_paper_seed{s}.csv", ["hash", "hash#aa", "hot_b2h", "hpb"], s,
              cache=512, runs=3, mode="interleave", extra=IL + ["--segments", "10000,30000"], tag="paper-dataset")
    # interleaved vs sequential measurement on the key configuration
    for s in SEEDS[:3]:
        qf = q_path("synth", 2_000_000, 0.3, "stationary", 1.2, 0.5, "ts", s)
        bench("B_modecheck", f"seq_s{s}", fib_path("synth", 2_000_000, 0.3), qf, ["hash", "hash#aa", "hpb"], s,
              runs=3, tag="mode=sequential")
        bench("B_modecheck", f"il_s{s}", fib_path("synth", 2_000_000, 0.3), qf, ["hash", "hash#aa", "hpb"], s,
              runs=3, mode="interleave", extra=IL, tag="mode=interleave")


def exp_stageC():
    """Stage C: incremental maintenance under FIB churn (interleaved, A/A, trie for dynamic guards)."""
    eng = ["hash", "hash#aa", "hpx:L=0/S=16", "hpx:L=0/S=16/INC=1", "pbc"]
    cfgs = [(500_000, a) for a in (1.0, 1.2)] + [(2_000_000, 1.2)]
    for n, alpha in cfgs:
        us = [1000, 5000, 25000, 100000] if n == 500_000 else [1000, 25000]
        for s in SEEDS[:3]:
            qf = q_path("synth", n, 0.3, "stationary", alpha, 0.5, "ts", s)
            bench("C_updates", f"n{n}_a{alpha}_U0_s{s}", fib_path("synth", n, 0.3), qf, eng[:4], s, runs=3,
                  mode="interleave", extra=["--window", "5000"], no_trie=False,
                  tag=f"n={n};alpha={alpha};U=0;target=none")
            for u, tgt in itertools.product(us, ["uniform", "hot"]):
                bench("C_updates", f"n{n}_a{alpha}_U{u}_{tgt}_s{s}", fib_path("synth", n, 0.3), qf, eng, s, runs=3,
                      mode="interleave", extra=["--window", str(min(u, 5000)), "--update-every", str(u),
                                                "--update-target", tgt], no_trie=False,
                      tag=f"n={n};alpha={alpha};U={u};target={tgt}")


EXPS = {"paper": exp_paper, "main": exp_main, "nest": exp_nest, "capacity": exp_capacity, "dynamics": exp_dynamics,
        "cutoff": exp_cutoff, "domain": exp_domain, "suffix": exp_suffix, "tail": exp_tail, "mem": exp_mem,
        "updates": exp_updates, "dynamics12": exp_dynamics12, "followup": exp_followup, "stageA": exp_stageA, "stageB": exp_stageB, "stageC": exp_stageC}

if __name__ == "__main__":
    for e in (sys.argv[1:] or list(EXPS)):
        log("=== experiment", e)
        EXPS[e]()
    log("=== all done")
