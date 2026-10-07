#!/usr/bin/env python3
"""Regression tests for research_run_manifest.py."""

from __future__ import annotations

import json
import subprocess
import tempfile
from pathlib import Path


SCRIPT = Path(__file__).with_name("research_run_manifest.py")


def run(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["python3", str(SCRIPT), *args],
        check=False,
        text=True,
        capture_output=True,
    )


def write_stats(path: Path, hotcold: int) -> None:
    path.write_text(
        "\n".join(
            [
                "compile_read_repromotion_enabled 0",
                "compile_die_batched_repromotion_enabled 0",
                f"compile_qlc_hotcold_enabled {hotcold}",
                "compile_qlc_rebalance_enabled 0",
                "compile_test_phase_repromotion_enabled 0",
                "compile_test_phase_qlc_rebalance_enabled 0",
                "read_priority_model nonpreemptive_submit_gate",
                "read_req_latency_count 3",
                "read_req_latency_hist_bins 2",
                "read_req_latency_hist_bin_00_upper_ns 1",
                "read_req_latency_hist_bin_00_count 1",
                "read_req_latency_hist_bin_01_upper_ns 3",
                "read_req_latency_hist_bin_01_count 2",
            ]
        )
        + "\n"
    )


def main() -> int:
    with tempfile.TemporaryDirectory() as tmpdir:
        root = Path(tmpdir)
        module = root / "nvmev.ko"
        module.write_bytes(b"module")
        stats = root / "stats.txt"
        write_stats(stats, hotcold=1)
        first = root / "first.json"
        second = root / "second.json"

        result = run(
            "capture",
            "--output",
            str(first),
            "--workload",
            "fio-mixed",
            "--variant",
            "die_latency1_qlc_all_norp_sb",
            "--module",
            str(module),
            "--stats",
            str(stats),
            "--param",
            "seed=42",
            "--strict-contract",
        )
        assert result.returncode == 0, result.stderr
        manifest = json.loads(first.read_text())
        assert manifest["compile_contract"]["status"] == "pass"
        assert manifest["model_contract"]["status"] == "pass"
        assert manifest["metric_contract"]["status"] == "pass"
        assert manifest["module"]["sha256"]

        result = run(
            "capture",
            "--output",
            str(second),
            "--workload",
            "fio-mixed",
            "--variant",
            "die_latency3_qlc_all_norp_sb",
            "--module",
            str(module),
            "--stats",
            str(stats),
            "--param",
            "seed=42",
            "--strict-contract",
        )
        assert result.returncode == 0, result.stderr
        assert run("compare", str(first), str(second)).returncode == 0

        write_stats(stats, hotcold=0)
        result = run(
            "capture",
            "--output",
            str(second),
            "--workload",
            "fio-mixed",
            "--variant",
            "die_latency3_qlc_all_norp_sb",
            "--stats",
            str(stats),
            "--strict-contract",
        )
        assert result.returncode == 2

    print("PASS research run manifest tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
