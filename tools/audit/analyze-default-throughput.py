# SPDX-License-Identifier: GPL-2.0
"""Summarize an interleaved current/old-default comparison, including paired rounds."""
import argparse
import csv
import hashlib
import importlib.util
import json
import pathlib
import statistics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=pathlib.Path)
    args = parser.parse_args()
    evidence_path = args.directory / "comparison-evidence.json"
    evidence = json.loads(evidence_path.read_text(encoding="utf-8-sig"))
    samples_path = args.directory / "comparison-samples.jsonl"
    samples = [json.loads(line) for line in samples_path.read_text(encoding="utf-8-sig").splitlines()]
    tests = json.loads((args.directory / "comparison-tests.json").read_text(encoding="utf-8-sig"))
    if tests["result"] != "passed" or tests["source_tree_sha256"] != evidence["source_tree_sha256"]:
        raise ValueError("Functional tests did not pass for the measured source snapshot")
    for artifact in evidence["artifacts"]:
        if hashlib.sha256(pathlib.Path(artifact["path"]).read_bytes()).hexdigest() != artifact["sha256"]:
            raise ValueError("Measured binary hash changed")
        if artifact["variant"] == "production" and artifact["compile_overrides"]:
            raise ValueError("Production compilation unexpectedly uses overrides")
    module_spec = importlib.util.spec_from_file_location(
        "exchange_analysis", pathlib.Path(__file__).with_name("analyze-exchange.py"))
    module = importlib.util.module_from_spec(module_spec)
    module_spec.loader.exec_module(module)
    summaries = module.aggregate([samples_path])
    summaries = {tuple(row[k] for k in module.KEYS): row for row in summaries}
    groups = {}
    for sample in samples:
        key = (sample["profile"], sample["direction"], sample["object_bytes"])
        groups.setdefault(key, []).append(sample)
    if len(groups) != 18 or len(samples) != evidence["samples"] or evidence["cases"] != 18:
        raise ValueError("Incomplete comparison matrix")
    expected_rounds = set(range(evidence["rounds"]))
    rows = []
    for (profile, direction, length), group in sorted(groups.items()):
        if {s["round"] for s in group} != expected_rounds or len(group) != len(expected_rounds):
            raise ValueError("Missing or duplicate comparison rounds")
        key = tuple(group[0][k] for k in module.KEYS)
        if any(tuple(s[k] for k in module.KEYS) != key for s in group):
            raise ValueError("Configuration changed within a profile")
        if any(s["config_init"] != (profile == "current_default") for s in group):
            raise ValueError("Unexpected default configuration selection")
        row = dict(profile=profile, **summaries[key])
        baseline = groups[("old_default", direction, length)]
        baseline_by_round = {s["round"]: s for s in baseline}
        old_rate = statistics.median(s["mobjects_per_second"] for s in baseline)
        paired = [s["mobjects_per_second"] / baseline_by_round[s["round"]]["mobjects_per_second"] for s in group]
        quartiles = statistics.quantiles(paired, n=4, method="inclusive") if len(paired) > 1 else paired * 3
        row.update(old_default_median_mobjects_s=old_rate,
                   ratio_of_medians=row["median_mobjects_s"] / old_rate,
                   improvement_percent=(row["median_mobjects_s"] / old_rate - 1) * 100,
                   paired_ratio_median=statistics.median(paired), paired_ratio_q1=quartiles[0],
                   paired_ratio_q3=quartiles[2])
        rows.append(row)
    output = args.directory / "comparison-summary.json"
    output.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    with output.with_suffix(".csv").open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    for row in rows:
        print(f"{row['profile']:18} {row['direction']} {row['object_bytes']:5}: "
              f"{row['median_mobjects_s']:8.3f} [{row['q1_mobjects_s']:.3f}, {row['q3_mobjects_s']:.3f}] "
              f"Mobj/s, {row['ratio_of_medians']:.2f}x old default, "
              f"paired median {row['paired_ratio_median']:.2f}x")
    print(f"Verified {len(samples)} samples, {len(rows)} groups and matching binary hashes; wrote {output}")


if __name__ == "__main__":
    main()
