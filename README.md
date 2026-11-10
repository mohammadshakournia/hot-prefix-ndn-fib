# Hot-prefix: guarded prefix caching for NDN FIB lookup

This repository contains the implementation, tests, experiment scripts and complete result sets for the paper

> M. R. Shakournia and N. Yazdani, *Hot-prefix: Guarded Prefix Caching with Exact Incremental Maintenance for
> NDN FIB Lookup*, under review.

Hot-prefix is a cache of resolved FIB prefixes that sits in front of an exact NDN FIB. Names that differ only in
a trailing component (a timestamp, a segment number) resolve to the same prefix; a cache hit avoids repeating the
longest-prefix match (LPM). Each cached prefix carries an exact *branch guard*, the set of next components under
which the FIB holds longer prefixes, so the cache returns the exact LPM result under every admission policy.

Main components (all in `src/fib_v3.hpp`):

- **Name front end:** one pass over the name in 8-byte words; component offsets and cumulative 64-bit prefix
  hashes, with no memory allocation. All engines share it.
- **Hot-prefix cache:** flat open-addressing index of resident prefixes probed from the longest resident depth
  (only the longest resident prefix can pass the guard), exact guards, sampled CLOCK replacement, and a
  TinyLFU-style frequency sketch keyed by the parser's prefix hash, observing one eligible miss in sixteen.
- **Incremental maintenance** under FIB updates (`onFibUpdate`): evict or refresh the updated prefix and rebuild
  the guards of its resident ancestors; the cache is never flushed.
- **Baselines on the same front end:** hash FIB, two-stage hash FIB (So et al., as in NDN-DPDK), exact full-name
  cache, Kim-style Bloom-filter prefix cache, PlusBitmap Caching (PBC), admit-all prefix cache, and the first
  prototype's engines (`v2_*`, from `third_party/fib_v2_common.hpp`).
- **Cost model** (`tools/model.py`): predicts the gain from quantities measurable on an existing FIB and gives a
  break-even hit ratio for enabling the cache.

## Repository layout

```
src/                 fib_v3.hpp (all engines), test_v3.cpp (correctness tests), bench_v3.cpp (benchmark harness),
                     profile_v3.cpp (cost-model inputs), micro_v3.cpp (cost breakdown)
scripts/             check_env.py, build.py, run_final.py, analyze_final.py, paper_figures.py   <- entry points
tools/               gen_v3.py (dataset generator), analyze.py (statistics), model.py (cost model)
tools/development/   harness used during development (kept for reference, see its README)
third_party/         first prototype (fib_v2_common.hpp) and its dataset generator
results/final/       the two result sets reported in the paper (platform-A, platform-B)
results/development/ every development experiment (Stages A-D)
figures/             figures of the paper (PDF)
```

## Requirements

- GCC 11 or newer (C++20). On Windows: MinGW-w64 GCC from [MSYS2](https://www.msys2.org) or WinLibs.
- Python 3.9+ with `numpy`, `matplotlib` and `psutil` (`pip install -r requirements.txt`).
- About 6 GB of free disk space for the generated datasets (`data/`, not committed) and 4 GB of RAM for the
  4M-prefix FIB.

## Quick start

```bash
pip install -r requirements.txt
python scripts/check_env.py           # records the platform, checks the toolchain
python scripts/build.py               # builds into build/ and runs the correctness tests
python scripts/run_final.py --quick   # reduced sanity run (about 30-60 minutes)
```

## Reproducing the paper

```bash
python scripts/run_final.py                      # all experiments F1-F12, several hours, resumable
python scripts/paper_figures.py --results results/final/<host>
```

`run_final.py` generates the datasets, pins the benchmark to one core (the last one by default, `--core N`),
raises its priority, and writes results, tables, figures and `SUMMARY.md` to `results/final/<host>/`.
Re-running it after an interruption resumes where it stopped; `--only F1 F6` repeats selected experiments.

The paper's figures were produced from the committed result sets with

```bash
python scripts/paper_figures.py --results results/final/platform-A --compare results/final/platform-B \
    --labels "Platform A: Xeon, 260 MiB LLC (cloud VM)|Platform B: Ryzen 5 5600H, 16 MiB LLC (laptop)"
```

and the tables with `python scripts/analyze_final.py --results results/final/platform-A` (and `platform-B`).

| id | question | configuration |
|---|---|---|
| F1 | where does the cache pay? | FIB 50K-4M x Zipf 0.8/1.0/1.2, 10 seeds x 3 runs, 9 engines |
| F2 | sequential vs interleaved measurement | 500K and 2M, Zipf 1.2 |
| F3 | prefix nesting | 500K, 0-50% aggregate prefixes |
| F4 | cache capacity | 2M, C = 256-16384 |
| F5 | non-stationary traffic | stationary, phase shift, scan, churn |
| F6 | FIB updates: flush vs incremental | one update every 1K-100K lookups, uniform / popularity-targeted |
| F7 | real names | 1M reversed domain names, rank popularity |
| F8 | first prototype's dataset | 50K, including the string-based prototype engines |
| F9 | tail latency | per-lookup TSC timing |
| F10 | memory | allocator-counted |
| F11 | cost model | per-lookup profiles for `tools/model.py` |
| F12 | FIB probes per lookup | deterministic counts |

## Measurement method

All engines advance through the same request stream in lockstep (`--mode interleave`, 5,000 lookups per slice,
random engine order per slice). Every run contains an identical second copy of the hash FIB (`hash#aa`) as an
A/A noise control. Every timed run is followed by an untimed replay that compares every result with an exact
reference; any mismatch aborts the run. Reductions are paired within runs, summarized per seed by the median,
and reported as the mean over seeds with a 95% percentile-bootstrap interval.

For stable timing: plug in a laptop and select the best-performance power mode, close other programs, and do
not use the machine during the run. On Linux, consider `cpupower frequency-set -g performance` and disabling turbo.

## Result sets

| directory | platform |
|---|---|
| `results/final/platform-A` | cloud VM, Intel Xeon 2.1 GHz (2 vCPUs), 260 MiB LLC, Linux 6.18, g++ 13.3 `-O3 -march=native` |
| `results/final/platform-B` | laptop, AMD Ryzen 5 5600H, 16 MiB L3, Windows 11, MinGW-w64 g++ 13.2 `-O3 -march=znver3 -static` (see `BUILD_NOTE.md`) |

Each contains the per-run CSVs of every experiment, `platform.json`, `run.log`, `tables/`, `figures/` and
`SUMMARY.md`. Per-window timing files are not committed (except the one used for the timeline figure).

## Benchmark engine names (`build/bench_v3 --engines ...`)

`trie hash hash2s full kim kim8 pbc` baselines, `hp` final Hot-prefix design, `hp_inc` the same with incremental
maintenance, `hp_2s` over the two-stage FIB, `hot_b0 hot_b1 hot_b2 hot_all` earlier tracker variants and the
admit-all ablation, `v2_trie v2_hash v2_hot_b1 v2_hot_b2` the first prototype, `<engine>#<tag>` an engine under a
second name (A/A control). Further options are listed at the top of `src/bench_v3.cpp`.

## Data sources

- Synthetic FIBs and request streams: `tools/gen_v3.py`.
- Domain FIB: the ranked top-1M domain list of https://github.com/zer0h/top-1000000-domains (downloaded on first
  use; SHA-256 recorded in `scripts/run_final.py`).

## Citation

See `CITATION.cff`. The citation will be updated when the paper is published.

## License

MIT, see `LICENSE`.
