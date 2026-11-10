# Development harness

These files were used during development stages A-D and produced
`results/development/`. They are kept for reference and are not needed to reproduce the paper (use the scripts in
`scripts/` instead).

- `run_all.py`: development experiment matrix. It expects the binaries in the repository root, the domain list in
  `../top-1000000-domains/` and a copy of the first prototype in `../v2ref/`.
- `quick.py`: quick paired summaries of a results directory.
- `run_profiles.sh`: cost-model profiles of the development runs.
- `ctl_debug.cpp`: diagnostics of the adaptive bypass controller of Stage B (not part of the final design);
  build with `g++ -std=c++20 -O2 tools/development/ctl_debug.cpp`.
