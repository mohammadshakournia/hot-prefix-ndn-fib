#!/usr/bin/env python3
"""Builds the programs into build/ and runs the correctness tests.

Usage: python scripts/build.py [--skip-tests] [--portable] [--cxx COMPILER]
  --portable  compile without -march=native (for running the binaries on another machine)
  --cxx       C++ compiler to use (default: $CXX, then g++); GCC 11 or newer is required (C++20)
"""
import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
EXE = ".exe" if sys.platform == "win32" else ""
PROGRAMS = [("test_v3", "test_v3.cpp", ["-O2"]),
            ("bench_v3", "bench_v3.cpp", []),
            ("bench_v3_mem", "bench_v3.cpp", ["-DFIBV3_MEMCOUNT"]),
            ("profile_v3", "profile_v3.cpp", []),
            ("micro_v3", "micro_v3.cpp", [])]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip-tests", action="store_true")
    ap.add_argument("--portable", action="store_true")
    ap.add_argument("--cxx", default=None)
    args = ap.parse_args()
    import os
    gxx = shutil.which(args.cxx or os.environ.get("CXX") or "g++")
    if not gxx:
        sys.exit("g++ not found; run scripts/check_env.py for installation instructions")
    ver = subprocess.run([gxx, "-dumpversion"], capture_output=True, text=True).stdout.strip()
    try:
        if int(ver.split(".")[0]) < 11:
            sys.exit(f"{gxx} is version {ver}; GCC 11 or newer is required (use --cxx to select another compiler)")
    except ValueError:
        pass
    BUILD.mkdir(exist_ok=True)
    arch = ["-march=x86-64-v2"] if args.portable else ["-march=native"]
    static = ["-static"] if sys.platform == "win32" else []
    for name, src, extra in PROGRAMS:
        opt = extra if "-O2" in extra else ["-O3"] + extra
        cmd = [gxx, "-std=c++20", *opt, *arch, *static, str(ROOT / "src" / src), "-o", str(BUILD / (name + EXE))]
        print(" ".join(cmd), flush=True)
        r = subprocess.run(cmd)
        if r.returncode:
            sys.exit(f"build of {name} failed")
    if not args.skip_tests:
        print("\nRunning correctness tests (a few minutes)...", flush=True)
        t = time.time()
        r = subprocess.run([str(BUILD / ("test_v3" + EXE))], capture_output=True, text=True)
        (BUILD / "test_output.txt").write_text(r.stdout + r.stderr)
        last = (r.stdout.strip().splitlines() or ["(no output)"])[-1]
        print(last, f"({time.time() - t:.0f} s; full log in build/test_output.txt)")
        if r.returncode:
            sys.exit("tests failed")
    print("\nBuild complete:", ", ".join(n + EXE for n, _, _ in PROGRAMS))


if __name__ == "__main__":
    main()
