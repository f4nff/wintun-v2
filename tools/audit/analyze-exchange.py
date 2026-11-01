# SPDX-License-Identifier: GPL-2.0
"""Aggregate executed exchange samples, retaining scope and measured variability."""
import argparse
import collections
import csv
import json
import pathlib
import statistics

KEYS = ("direction", "region_limit_bytes", "token_validation", "max_batch", "batch", "queues", "slots",
        "ring", "byte_budget", "object_bytes")


def aggregate(paths):
    groups = collections.defaultdict(list)
    for path in paths:
        for line in path.read_text(encoding="utf-8-sig").splitlines():
            sample = json.loads(line)
            if sample.get("exit_code", 0) != 0:
                raise ValueError(f"Failed sample in {path}")
            sample.setdefault("region_limit_bytes", 64 * 1024 * 1024)
            sample.setdefault("token_validation", "filter")
            groups[tuple(sample[k] for k in KEYS)].append(sample)
    result = []
    for key, samples in groups.items():
        rounds = [s["round"] for s in samples]
        if len(set(rounds)) != len(rounds):
            raise ValueError(f"Duplicate rounds for {key}; do not mix screening and confirmation")
        rates = sorted(s["mobjects_per_second"] for s in samples)
        quartiles = statistics.quantiles(rates, n=4, method="inclusive") if len(rates) > 1 else rates * 3
        row = dict(zip(KEYS, key), samples=len(samples),
                   median_mobjects_s=statistics.median(rates), min_mobjects_s=rates[0], max_mobjects_s=rates[-1],
                   q1_mobjects_s=quartiles[0], q3_mobjects_s=quartiles[2],
                   median_cycles_object=statistics.median(s["cycles_per_object"] for s in samples),
                   median_set_calls_per_million=statistics.median(
                       (s["user_set_events"] + s["peer_set_events"]) / s["objects"] * 1e6 for s in samples),
                   median_wait_calls_per_million=statistics.median(
                       (s["user_waits"] + s["peer_waits"]) / s["objects"] * 1e6 for s in samples),
                   median_user_batch=statistics.median(s["objects"] / s["user_batches"] for s in samples),
                   median_peer_batch=statistics.median(s["objects"] / s["peer_batches"] for s in samples),
                   region_bytes=samples[0]["region_bytes"], objects_per_sample=samples[0]["objects"])
        if any(s["objects"] != row["objects_per_sample"] for s in samples):
            raise ValueError("Unequal sample lengths within a configuration")
        result.append(row)
    return sorted(result, key=lambda r: tuple(r[k] for k in KEYS))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", type=pathlib.Path, nargs="+")
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--expected-rounds", type=int, help="Reject incomplete configuration groups")
    parser.add_argument("--candidates", type=pathlib.Path,
                        help="Write best per direction/length/batch plus baseline for longer confirmation")
    args = parser.parse_args()
    rows = aggregate(args.inputs)
    if not rows or (args.expected_rounds is not None and any(r["samples"] != args.expected_rounds for r in rows)):
        raise ValueError("No samples or incomplete configuration groups")
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "summary.json").write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    with (args.output / "summary.csv").open("w", newline="", encoding="utf-8-sig") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    selected = {}
    for direction in ("rx", "tx"):
        for length in (64, 1500, 32768):
            matching = [r for r in rows if r["direction"] == direction and r["object_bytes"] == length]
            if not matching:
                continue
            for width in (32, 64, 128, 256):
                eligible = [r for r in matching if r["max_batch"] == width and r["batch"] == width]
                if eligible:
                    best = max(eligible, key=lambda r: r["median_mobjects_s"])
                    selected[tuple(best[k] for k in KEYS)] = best
            # Keep the current byte budget independently of test-only wider budgets.
            eligible = [r for r in matching if r["max_batch"] == 32 and r["byte_budget"] == 131072]
            if eligible:
                best = max(eligible, key=lambda r: r["median_mobjects_s"])
                selected[tuple(best[k] for k in KEYS)] = best
            for r in matching:
                if (r["max_batch"], r["batch"], r["queues"], r["slots"], r["ring"], r["byte_budget"]) == (32, 32, 1, 32, 64, 131072):
                    selected[tuple(r[k] for k in KEYS)] = r
            top = max(matching, key=lambda r: r["median_mobjects_s"])
            print(f"{direction} {length}: {top['median_mobjects_s']:.3f} Mobj/s; "
                  f"region={top['region_limit_bytes']//1048576} MiB batch={top['batch']} "
                  f"q={top['queues']} slots={top['slots']} ring={top['ring']} "
                  f"budget={top['byte_budget']//1024} KiB n={top['samples']}")
    if args.candidates:
        # Region is a compile parameter, so only mix one limit per candidate file.
        limits = {r["region_limit_bytes"] for r in selected.values()}
        if len(limits) != 1:
            raise ValueError("Generate candidates separately for each region limit")
        args.candidates.write_text(json.dumps([{k: r[k] for k in KEYS} for r in selected.values()], indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {len(rows)} configuration summaries to {args.output}")


if __name__ == "__main__":
    main()
