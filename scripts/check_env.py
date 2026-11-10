#!/usr/bin/env python3
"""Records the platform and checks the toolchain. Writes <out>/platform.json.

Usage: python scripts/check_env.py [--out results/final/<host>]
"""
import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=30, shell=isinstance(cmd, str)).stdout.strip()
    except Exception as e:  # noqa: BLE001
        return f"unavailable ({e.__class__.__name__})"


def cpu_info():
    info = {"logical_cpus": os.cpu_count()}
    if sys.platform == "win32":
        ps = ("Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors,"
              "L2CacheSize,L3CacheSize,MaxClockSpeed | ConvertTo-Json")
        out = run(["powershell", "-NoProfile", "-Command", ps])
        try:
            info["cpu"] = json.loads(out)
        except Exception:  # noqa: BLE001
            info["cpu_raw"] = out
        info["memory"] = run(["powershell", "-NoProfile", "-Command",
                              "(Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory"])
        info["power_scheme"] = run(["powercfg", "/getactivescheme"])
    else:
        info["lscpu"] = run(["lscpu"])
        info["memory"] = run(["sh", "-c", "grep MemTotal /proc/meminfo"])
        gov = Path("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")
        info["governor"] = gov.read_text().strip() if gov.exists() else "n/a"
        nt = Path("/sys/devices/system/cpu/intel_pstate/no_turbo")
        info["intel_no_turbo"] = nt.read_text().strip() if nt.exists() else "n/a"
    return info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "results" / "final" / platform.node()))
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    gxx = shutil.which("g++")
    mods = {}
    for m in ("numpy", "matplotlib", "psutil"):
        try:
            mods[m] = __import__(m).__version__
        except Exception:  # noqa: BLE001
            mods[m] = None
    env = {
        "host": platform.node(), "os": platform.platform(), "python": sys.version.split()[0], "modules": mods,
        "gxx_path": gxx, "gxx_version": run([gxx, "--version"]).splitlines()[0] if gxx else None,
        "git": run(["git", "--version"]) if shutil.which("git") else None,
        **cpu_info(),
    }
    (out / "platform.json").write_text(json.dumps(env, indent=1))
    print(json.dumps(env, indent=1)[:3000])
    problems = []
    if not gxx:
        problems.append("g++ not found. Windows: install MSYS2 (https://www.msys2.org), then in the 'MSYS2 UCRT64' shell run "
                        "'pacman -S mingw-w64-ucrt-x86_64-gcc' and add C:\\msys64\\ucrt64\\bin to PATH. Linux: install g++ >= 11.")
    else:
        v = env["gxx_version"] or ""
        try:
            major = int(v.split()[-1].split(".")[0]) if v else 0
        except ValueError:
            major = 0
        if major and major < 11:
            problems.append(f"g++ {major} is too old; C++20 (<bit>, concepts) needs g++ >= 11.")
    for m in ("numpy", "matplotlib"):
        if not mods[m]:
            problems.append(f"Python module {m} missing (pip install {m}).")
    if not mods["psutil"]:
        problems.append("psutil missing: experiments will run without core pinning (pip install psutil).")
    print("\nOK" if not problems else "\nPROBLEMS:\n- " + "\n- ".join(problems))
    print(f"\nPlatform record written to {out / 'platform.json'}")
    return 1 if any("not found" in p or "too old" in p or "missing (pip" in p for p in problems) else 0


if __name__ == "__main__":
    sys.exit(main())
