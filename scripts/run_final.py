#!/usr/bin/env python3
"""Final experiment set for the paper, to run on a dedicated machine.

Usage:
  python scripts/run_final.py --quick          # ~15-30 min sanity run (reduced seeds and configurations)
  python scripts/run_final.py                  # full set (several hours; resumable)
  python scripts/run_final.py --only F1 F6     # selected experiments
Options: --core N (CPU core to pin to; default: the last core), --out DIR

Every experiment is resumable: an output file that already exists is skipped.
Results go to results/final/<host>/ (raw CSVs), then analysis writes tables/ and figures/ there.

Experiments
  F1  main grid: FIB 50K-4M x Zipf alpha 0.8/1.0/1.2, 10 seeds, interleaved, A/A control
  F2  sequential vs interleaved measurement (2M and 500K, alpha 1.2)
  F3  nesting sweep (500K, nest 0-0.5)
  F4  cache capacity (2M, C 256-16384)
  F5  non-stationary workloads (2M; stationary, phase shift, scan, churn)
  F6  FIB churn: flush-on-update vs incremental maintenance
  F7  domain-name FIB (1M reversed domains)
  F8  first-prototype dataset incl. the prototype (v2) engines
  F9  per-lookup tail latency
  F10 allocator-counted memory
  F11 cost-model profiles (Stage D)
  F12 FIB probes per lookup (deterministic, machine independent)
"""
import argparse
import itertools
import os
import platform
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
EXE = ".exe" if sys.platform == "win32" else ""
DATA = ROOT / "data"
GEN = ROOT / "tools" / "gen_v3.py"
V2GEN = ROOT / "third_party" / "v2" / "generate_ndn_datasets.py"
DOMAIN_URL = "https://raw.githubusercontent.com/zer0h/top-1000000-domains/master/top-1000000-domains"
DOMAIN_FILE = DATA / "top-1000000-domains.txt"
DOMAIN_SHA256 = "f20cbd5f1b4d975135d6ccf7fe776e4ce681f86003641e70df651eb98824362c"
Q = 300_000

try:
    import psutil
except ImportError:  # pinning becomes a no-op
    psutil = None

ARGS = None
LOG = None


def log(*a):
    msg = time.strftime("%Y-%m-%d %H:%M:%S ") + " ".join(str(x) for x in a)
    print(msg, flush=True)
    LOG.write(msg + "\n")
    LOG.flush()


# ---------------------------------------------------------------- datasets
def fib_path(family, n, nest):
    return DATA / (f"{family}_n{n}_nest{nest}.csv" if family == "synth" else f"domain_n{n}.csv")


def q_path(family, n, nest, kind, alpha, suffix, seed):
    tag = f"{family}_n{n}_nest{nest}" if family == "synth" else f"domain_n{n}"
    return DATA / f"q_{tag}_{kind}_a{alpha}_r0.5_{suffix}_c{Q}_s{seed}.csv"


def domain_list():
    if DOMAIN_FILE.exists():
        return DOMAIN_FILE
    log("downloading the domain list", DOMAIN_URL)
    urllib.request.urlretrieve(DOMAIN_URL, DOMAIN_FILE)
    import hashlib
    h = hashlib.sha256(DOMAIN_FILE.read_bytes()).hexdigest()
    if h != DOMAIN_SHA256:
        log("WARNING: domain list checksum differs from the one used in the cloud runs:", h)
    return DOMAIN_FILE


def ensure(family, n, nest, streams):
    """streams: (kind, alpha, suffix, seed) tuples."""
    DATA.mkdir(exist_ok=True)
    fib = fib_path(family, n, nest)
    specs = [f"{k}:{a}:0.5:{sfx}:{Q}:{s}:{q_path(family, n, nest, k, a, sfx, s)}"
             for k, a, sfx, s in streams if not q_path(family, n, nest, k, a, sfx, s).exists()]
    if fib.exists() and not specs:
        return
    cmd = [sys.executable, str(GEN), "--family", family, "--fib-size", str(n), "--out-fib", str(fib)]
    cmd += ["--nest", str(nest)] if family == "synth" else ["--domains", str(domain_list())]
    if specs:
        cmd += ["--queries"] + specs
    log("generate", family, n, nest, f"{len(specs)} streams")
    subprocess.run(cmd, check=True)


def paper_data(seed):
    fib = DATA / "paper" / "fib_50000.csv"
    qf = DATA / "paper" / f"queries_stationary_fib50000_q100000_seed{seed}.csv"
    if not qf.exists():
        subprocess.run([sys.executable, str(V2GEN), "--fib-sizes", "50000", "--query-count", "100000", "--workloads",
                        "stationary", "--seeds", str(seed), "--output-dir", str(DATA / "paper")], check=True,
                       stdout=subprocess.DEVNULL)
    return fib, qf


# ---------------------------------------------------------------- running
def pinned(cmd, stdout=subprocess.DEVNULL):
    p = subprocess.Popen(cmd, stdout=stdout, stderr=subprocess.PIPE, text=True)
    if psutil:
        try:
            proc = psutil.Process(p.pid)
            proc.cpu_affinity([ARGS.core])
            if sys.platform == "win32":
                proc.nice(psutil.HIGH_PRIORITY_CLASS)
            else:
                try:
                    proc.nice(-5)
                except psutil.AccessDenied:
                    pass
        except Exception as e:  # noqa: BLE001
            log("pinning failed:", e)
    _, err = p.communicate()
    return p.returncode, err


def bench(exp, name, fib, qf, engines, seed, cache=1024, runs=3, mode="interleave", window=5000, extra=(), tag="",
          mem=False, trie=False):
    outdir = ARGS.out / exp
    outdir.mkdir(parents=True, exist_ok=True)
    prefix = outdir / name
    if Path(str(prefix) + "_runs.csv").exists():
        return
    binary = BUILD / (("bench_v3_mem" if mem else "bench_v3") + EXE)
    cmd = [str(binary), "--fib", str(fib), "--queries", str(qf), "--engines", ",".join(engines), "--runs", str(runs),
           "--seed", str(seed), "--cache", str(cache), "--mode", mode, "--window", str(window),
           "--segments", "50000,100000", "--tag", tag or name, "--out", str(prefix), *extra]
    if not trie:
        cmd.append("--no-trie")
    t = time.time()
    rc, err = pinned(cmd)
    if rc:
        log("FAILED", exp, name, err[-3000:])
        sys.exit(1)
    log("done", exp, name, f"{time.time() - t:.0f}s")


def seeds(full, quick):
    return list(range(1, (quick if ARGS.quick else full) + 1))


# ---------------------------------------------------------------- experiments
def F1():
    ns = [50_000, 2_000_000] if ARGS.quick else [50_000, 500_000, 2_000_000, 4_000_000]
    eng = ["hash", "hash#aa", "hash2s", "full", "kim", "pbc", "hot_b2", "hot_all", "hp"]
    for n, a in itertools.product(ns, [0.8, 1.0, 1.2]):
        ss = seeds(10, 2)
        ensure("synth", n, 0.3, [("stationary", a, "ts", s) for s in ss])
        for s in ss:
            bench("F1_main", f"n{n}_a{a}_s{s}", fib_path("synth", n, 0.3), q_path("synth", n, 0.3, "stationary", a, "ts", s),
                  eng, s, tag=f"n={n};alpha={a}")


def F2():
    for n in ([2_000_000] if ARGS.quick else [500_000, 2_000_000]):
        ensure("synth", n, 0.3, [("stationary", 1.2, "ts", s) for s in seeds(5, 2)])
        for s in seeds(5, 2):
            qf = q_path("synth", n, 0.3, "stationary", 1.2, "ts", s)
            for mode in ["timeline", "interleave"]:
                bench("F2_modecheck", f"n{n}_{mode}_s{s}", fib_path("synth", n, 0.3), qf, ["hash", "hash#aa", "hp"], s,
                      mode=mode, tag=f"n={n};mode={mode}")


def F3():
    for nest, a in itertools.product([0.0, 0.1, 0.3, 0.5], [1.0, 1.2]):
        if ARGS.quick and nest in (0.1, 0.5):
            continue
        ensure("synth", 500_000, nest, [("stationary", a, "ts", s) for s in seeds(5, 1)])
        for s in seeds(5, 1):
            bench("F3_nest", f"nest{nest}_a{a}_s{s}", fib_path("synth", 500_000, nest),
                  q_path("synth", 500_000, nest, "stationary", a, "ts", s),
                  ["hash", "hash#aa", "kim", "kim8", "pbc", "hp"], s, tag=f"nest={nest};alpha={a}")


def F4():
    for a, c in itertools.product([1.0, 1.2], [256, 1024, 4096, 16384]):
        ensure("synth", 2_000_000, 0.3, [("stationary", a, "ts", s) for s in seeds(5, 1)])
        for s in seeds(5, 1):
            bench("F4_capacity", f"a{a}_C{c}_s{s}", fib_path("synth", 2_000_000, 0.3),
                  q_path("synth", 2_000_000, 0.3, "stationary", a, "ts", s), ["hash", "hash#aa", "kim", "pbc", "hp"], s,
                  cache=c, tag=f"alpha={a};C={c}")


def F5():
    for k, a in itertools.product(["stationary", "phaseshift", "scan", "churn"], [1.0, 1.2]):
        ensure("synth", 2_000_000, 0.3, [(k, a, "ts", s) for s in seeds(5, 1)])
        for s in seeds(5, 1):
            bench("F5_dynamics", f"{k}_a{a}_s{s}", fib_path("synth", 2_000_000, 0.3),
                  q_path("synth", 2_000_000, 0.3, k, a, "ts", s), ["hash", "hash#aa", "hot_b2", "hp"], s,
                  tag=f"workload={k};alpha={a}")


def F6():
    cfgs = [(500_000, 1.2)] if ARGS.quick else [(500_000, 1.0), (500_000, 1.2), (2_000_000, 1.2)]
    for n, a in cfgs:
        us = [1000, 25000] if (ARGS.quick or n == 2_000_000) else [1000, 5000, 25000, 100000]
        ensure("synth", n, 0.3, [("stationary", a, "ts", s) for s in seeds(3, 1)])
        for s in seeds(3, 1):
            qf = q_path("synth", n, 0.3, "stationary", a, "ts", s)
            bench("F6_updates", f"n{n}_a{a}_U0_s{s}", fib_path("synth", n, 0.3), qf, ["hash", "hash#aa", "hp", "hp_inc"], s,
                  trie=True, tag=f"n={n};alpha={a};U=0;target=none")
            for u, tgt in itertools.product(us, ["uniform", "hot"]):
                bench("F6_updates", f"n{n}_a{a}_U{u}_{tgt}_s{s}", fib_path("synth", n, 0.3), qf,
                      ["hash", "hash#aa", "hp", "hp_inc", "pbc"], s, window=min(u, 5000), trie=True,
                      extra=["--update-every", str(u), "--update-target", tgt], tag=f"n={n};alpha={a};U={u};target={tgt}")


def F7():
    for a in [0.8, 1.0, 1.2]:
        ensure("domain", 1_000_000, 0, [("stationary", a, "ts", s) for s in seeds(5, 1)])
        for s in seeds(5, 1):
            bench("F7_domain", f"a{a}_s{s}", fib_path("domain", 1_000_000, 0),
                  q_path("domain", 1_000_000, 0, "stationary", a, "ts", s),
                  ["hash", "hash#aa", "hash2s", "full", "kim", "pbc", "hp", "hp_2s"], s, tag=f"alpha={a}")


def F8():
    for s in seeds(5, 1):
        fib, qf = paper_data(s)
        bench("F8_paper", f"s{s}", fib, qf, ["hash", "hash#aa", "trie", "kim", "pbc", "hot_b1", "hot_b2", "hp", "v2_trie",
                                             "v2_hash", "v2_hot_b1", "v2_hot_b2"], s, cache=512, trie=True,
              extra=["--segments", "10000,30000"], tag="paper-dataset")


def F9():
    for a in [1.0, 1.2]:
        for s in seeds(3, 1):
            ensure("synth", 2_000_000, 0.3, [("stationary", a, "ts", s)])
            bench("F9_tail", f"a{a}_s{s}", fib_path("synth", 2_000_000, 0.3),
                  q_path("synth", 2_000_000, 0.3, "stationary", a, "ts", s), ["hash", "hash2s", "pbc", "hp"], s, runs=3,
                  mode="tail", tag=f"alpha={a}")


def F10():
    for n, c in itertools.product([500_000, 2_000_000], [1024, 16384]):
        if ARGS.quick and n == 2_000_000:
            continue
        ensure("synth", n, 0.3, [("stationary", 1.0, "ts", 1)])
        bench("F10_mem", f"n{n}_C{c}", fib_path("synth", n, 0.3), q_path("synth", n, 0.3, "stationary", 1.0, "ts", 1),
              ["hash", "full", "kim", "pbc", "hot_b0", "hot_b1", "hot_b2", "hp"], 1, cache=c, runs=1, mode="timeline",
              mem=True, trie=(n <= 500_000), tag=f"n={n};C={c}")


def F11():
    out = ARGS.out / "D_profiles.csv"
    if not out.exists():
        out.write_text("tag,C,rep,n,h,F_hot,F_cold,T_hash,c_hit,c_miss,T_cache\n")
    done = out.read_text()
    jobs = []
    ns = [50_000, 2_000_000] if ARGS.quick else [50_000, 500_000, 2_000_000, 4_000_000]
    for n, a in itertools.product(ns, [0.8, 1.0, 1.2]):
        for s in seeds(3, 1):
            jobs.append(("synth", n, 0.3, a, s, 1024))
    for a in [0.8, 1.0, 1.2]:
        for s in seeds(3, 1):
            jobs.append(("domain", 1_000_000, 0, a, s, 1024))
    if not ARGS.quick:
        for c in [256, 4096, 16384]:
            for s in seeds(2, 1):
                jobs.append(("synth", 2_000_000, 0.3, 1.2, s, c))
        for nest in [0.0, 0.5]:
            for s in seeds(2, 1):
                jobs.append(("synth", 500_000, nest, 1.2, s, 1024))
    for fam, n, nest, a, s, c in jobs:
        tag = f"{fam};n={n};alpha={a};nest={nest if fam == 'synth' else 'dom'};C={c};seed={s}"
        if tag + "," in done:
            continue
        ensure(fam, n, nest, [("stationary", a, "ts", s)])
        t = time.time()
        rc, err = pinned([str(BUILD / ("profile_v3" + EXE)), str(fib_path(fam, n, nest)),
                          str(q_path(fam, n, nest, "stationary", a, "ts", s)), str(c), "2", str(out), tag])
        if rc:
            log("FAILED profile", tag, err[-2000:])
            sys.exit(1)
        log("done F11", tag, f"{time.time() - t:.0f}s")


def F12():
    """Deterministic FIB probes per lookup (machine independent): sequential, one run."""
    ns = [50_000, 2_000_000] if ARGS.quick else [50_000, 500_000, 2_000_000, 4_000_000]
    for n, a in itertools.product(ns, [0.8, 1.0, 1.2]):
        ensure("synth", n, 0.3, [("stationary", a, "ts", 1)])
        bench("F12_probes", f"n{n}_a{a}", fib_path("synth", n, 0.3), q_path("synth", n, 0.3, "stationary", a, "ts", 1),
              ["hash", "hash2s", "kim", "pbc", "hp"], 1, runs=1, mode="timeline", tag=f"n={n};alpha={a}")


EXPERIMENTS = {"F1": F1, "F2": F2, "F3": F3, "F4": F4, "F5": F5, "F6": F6, "F7": F7, "F8": F8, "F9": F9,
               "F10": F10, "F11": F11, "F12": F12}


def main():
    global ARGS, LOG
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--only", nargs="*", default=None)
    ap.add_argument("--core", type=int, default=(os.cpu_count() or 1) - 1)
    ap.add_argument("--out", default=None)
    ap.add_argument("--no-analysis", action="store_true")
    ARGS = ap.parse_args()
    host = platform.node() + ("-quick" if ARGS.quick else "")
    ARGS.out = Path(ARGS.out) if ARGS.out else ROOT / "results" / "final" / host
    ARGS.out.mkdir(parents=True, exist_ok=True)
    LOG = open(ARGS.out / "run.log", "a")
    for prog in ["bench_v3", "bench_v3_mem", "profile_v3"]:
        if not (BUILD / (prog + EXE)).exists():
            sys.exit(f"build/{prog}{EXE} missing: run python scripts/build.py first")
    if not (ARGS.out / "platform.json").exists():
        subprocess.run([sys.executable, str(ROOT / "scripts" / "check_env.py"), "--out", str(ARGS.out)],
                       stdout=subprocess.DEVNULL)
    log("=== start", "quick" if ARGS.quick else "full", "core", ARGS.core, "psutil" if psutil else "no pinning",
        "->", ARGS.out)
    for name in (ARGS.only or list(EXPERIMENTS)):
        log("=== experiment", name)
        EXPERIMENTS[name]()
    log("=== all experiments done")
    if not ARGS.no_analysis:
        subprocess.run([sys.executable, str(ROOT / "scripts" / "analyze_final.py"), "--results", str(ARGS.out)])


if __name__ == "__main__":
    main()
