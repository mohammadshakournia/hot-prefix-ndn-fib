#!/usr/bin/env python3
"""Generate scalable synthetic NDN FIB and query datasets.

The workload definitions match validate_b2_v2.cpp conceptually:
stationary (Zipf 1.0), phase shift (two independently ranked Zipf phases),
scan (Zipf -> one-off uniform scan -> Zipf recovery), and churn
(70% broad one-off traffic mixed with 30% Zipf 1.5 traffic).

Only Python's standard library is required.
FIB prefix depths vary deterministically between 3 and 10 components.
Query suffixes are appended after the full FIB prefix.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import json
import math
import random
import zlib
from pathlib import Path


def parse_int_list(text: str) -> list[int]:
    values = [int(x.strip()) for x in text.split(",") if x.strip()]
    if not values or any(x <= 0 for x in values):
        raise argparse.ArgumentTypeError("expected comma-separated positive integers")
    return values


def parse_str_list(text: str) -> list[str]:
    values = [x.strip().lower() for x in text.split(",") if x.strip()]
    allowed = {"stationary", "phaseshift", "scan", "churn"}
    unknown = set(values) - allowed
    if not values or unknown:
        raise argparse.ArgumentTypeError(f"unknown workloads: {sorted(unknown)}")
    return values


def prefix(index: int) -> str:
    """Return a unique prefix with 3..10 components, stable across runs.

    Hashing the index avoids tying depth directly to the domain index.
    The final s{index} component guarantees uniqueness at every depth.
    """
    depth = 3 + zlib.crc32(str(index).encode("ascii")) % 8
    middle = [
        "service",
        f"region{(index // 1024) % 16}",
        f"site{(index // 16384) % 32}",
        f"app{index % 16}",
        f"collection{(index // 16) % 64}",
        f"version{(index // 64) % 8}",
        f"shard{(index // 8) % 32}",
    ]
    components = ["ndn", f"d{index % 1024}"]
    components.extend(middle[:depth - 3])
    components.append(f"s{index}")
    return "/" + "/".join(components)


def write_fib(path: Path, fib_size: int) -> None:
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f, lineterminator="\n")
        writer.writerow(["prefix", "face"])
        for i in range(fib_size):
            writer.writerow([prefix(i), i % 4096])


def shuffled_order(fib_size: int, seed: int) -> list[int]:
    order = list(range(fib_size))
    random.Random(seed ^ 0x9E3779B97F4A7C15).shuffle(order)
    return order


def zipf_cdf(fib_size: int, exponent: float) -> list[float]:
    cdf: list[float] = []
    total = 0.0
    for rank in range(1, fib_size + 1):
        total += 1.0 / math.pow(rank, exponent)
        cdf.append(total)
    return [x / total for x in cdf]


def zipf_queries(fib_size: int, count: int, exponent: float, rank_seed: int,
                 draw_seed: int, start_index: int = 0):
    order = shuffled_order(fib_size, rank_seed)
    cdf = zipf_cdf(fib_size, exponent)
    rng = random.Random(draw_seed)
    for local_i in range(count):
        i = start_index + local_i
        rank = bisect.bisect_left(cdf, rng.random())
        yield f"{prefix(order[min(rank, fib_size - 1)])}/object/{i % 257}"


def uniform_scan_queries(fib_size: int, count: int, seed: int, start_index: int = 0):
    rng = random.Random(seed)
    for local_i in range(count):
        i = start_index + local_i
        yield f"{prefix(rng.randrange(fib_size))}/scan/{i}"


def workload_queries(kind: str, fib_size: int, count: int, seed: int):
    if kind == "stationary":
        yield from zipf_queries(fib_size, count, 1.0, seed, seed)
        return

    if kind == "phaseshift":
        first = count // 2
        yield from zipf_queries(fib_size, first, 1.0, seed, seed)
        second_seed = seed + 1_000_003
        yield from zipf_queries(fib_size, count - first, 1.0, second_seed, second_seed,
                                start_index=first)
        return

    if kind == "scan":
        first = count // 4
        middle = count // 2
        yield from zipf_queries(fib_size, first, 1.0, seed, seed)
        yield from uniform_scan_queries(fib_size, middle, seed + 7, start_index=first)
        yield from zipf_queries(fib_size, count - first - middle, 1.0, seed,
                                seed + 1_000_033, start_index=first + middle)
        return

    if kind == "churn":
        broad_count = 7 * count // 10
        hot_count = count - broad_count
        broad = iter(uniform_scan_queries(fib_size, broad_count, seed + 11))
        hot = iter(zipf_queries(fib_size, hot_count, 1.5, seed + 13, seed + 13))
        rng = random.Random(seed + 31)
        broad_left, hot_left = broad_count, hot_count
        while broad_left or hot_left:
            take_broad = hot_left == 0 or (broad_left > 0 and rng.randrange(10) < 7)
            if take_broad:
                yield next(broad)
                broad_left -= 1
            else:
                yield next(hot)
                hot_left -= 1
        return

    raise ValueError(kind)


def write_queries(path: Path, kind: str, fib_size: int, count: int, seed: int) -> None:
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f, lineterminator="\n")
        writer.writerow(["query"])
        writer.writerows((q,) for q in workload_queries(kind, fib_size, count, seed))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fib-sizes", type=parse_int_list,
                        default=parse_int_list("10000,50000,100000,500000,1000000"))
    parser.add_argument("--query-count", type=int, default=1_000_000)
    parser.add_argument("--workloads", type=parse_str_list,
                        default=parse_str_list("stationary,phaseshift,scan,churn"))
    parser.add_argument("--seeds", type=parse_int_list, default=[1])
    parser.add_argument("--output-dir", type=Path, default=Path("datasets"))
    args = parser.parse_args()
    if args.query_count <= 0:
        parser.error("--query-count must be positive")

    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    manifest = {
        "fib_sizes": args.fib_sizes,
        "query_count": args.query_count,
        "workloads": args.workloads,
        "seeds": args.seeds,
        "prefix_depth_min": 3,
        "prefix_depth_max": 10,
        "prefix_depth_assignment": "3 + crc32(decimal index) % 8",
        "files": [],
    }

    for fib_size in args.fib_sizes:
        fib_path = out / f"fib_{fib_size}.csv"
        print(f"Writing {fib_path.name} ...", flush=True)
        write_fib(fib_path, fib_size)
        manifest["files"].append(str(fib_path.name))
        for seed in args.seeds:
            for kind in args.workloads:
                query_path = out / f"queries_{kind}_fib{fib_size}_q{args.query_count}_seed{seed}.csv"
                print(f"Writing {query_path.name} ...", flush=True)
                write_queries(query_path, kind, fib_size, args.query_count, seed)
                manifest["files"].append(str(query_path.name))

    manifest_path = out / "dataset_manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(f"Done. Manifest: {manifest_path}")


if __name__ == "__main__":
    main()
