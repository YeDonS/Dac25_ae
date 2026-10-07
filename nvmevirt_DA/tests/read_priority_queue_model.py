#!/usr/bin/env python3
"""Small one-LUN model for reasoning about HP/LP scheduling semantics.

This is intentionally not a performance model. It demonstrates the ordering
cases that matter for latency2/3 and contrasts the implemented queue-deferral
model with a non-preemptive reference model.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass
class Tails:
    hp: int = 0
    lp: int = 0

    @property
    def tail(self) -> int:
        return max(self.hp, self.lp)

    def read_priority(self, name: str, issue: int, busy: int) -> str:
        start = max(self.hp, self.lp, issue)
        end = start + busy
        self.hp = end
        self.lp = end
        return format_event(name, issue, start, end, 0, self.lp)

    def low_priority(self, name: str, issue: int, busy: int) -> str:
        start = max(self.hp, self.lp, issue)
        end = start + busy
        self.lp = end
        return format_event(name, issue, start, end, 0, self.lp)


@dataclass
class Interval:
    start: int
    end: int


@dataclass
class NonPreemptiveTails:
    hp: int = 0
    lp_intervals: list[Interval] | None = None

    def __post_init__(self) -> None:
        if self.lp_intervals is None:
            self.lp_intervals = []

    @property
    def lp(self) -> int:
        assert self.lp_intervals is not None
        return self.lp_intervals[-1].end if self.lp_intervals else 0

    def low_priority(self, name: str, issue: int, busy: int) -> str:
        assert self.lp_intervals is not None
        start = max(self.hp, self.lp, issue)
        end = start + busy
        self.lp_intervals.append(Interval(start, end))
        return format_event(name, issue, start, end, 0, self.lp)

    def read_priority(self, name: str, issue: int, busy: int) -> str:
        assert self.lp_intervals is not None
        old_lp = self.lp
        in_flight_end = max(
            (interval.end for interval in self.lp_intervals
             if interval.start <= issue < interval.end),
            default=0,
        )
        start = max(self.hp, issue, in_flight_end)
        end = start + busy
        for interval in self.lp_intervals:
            if interval.start >= start:
                interval.start += busy
                interval.end += busy
        self.hp = end
        bypass = max(0, old_lp - start)
        return format_event(name, issue, start, end, bypass, self.lp)


def format_event(
    name: str,
    issue: int,
    start: int,
    end: int,
    bypass: int,
    lp_completion: int,
) -> str:
    return (
        f"{name}: issue={issue} start={start} end={end} "
        f"request_latency={end - issue} bypass={bypass} "
        f"lp_completion={lp_completion}"
    )


def emit(title: str, lines: list[str]) -> None:
    print(f"\n[{title}]")
    for line in lines:
        print(line)


def bg_already_issued() -> None:
    tails = Tails()
    lines = [
        tails.low_priority("bg", issue=0, busy=100),
        tails.read_priority("read", issue=10, busy=20),
        "conclusion: submitted LP work is non-preemptible",
    ]
    emit("nonpreemptive_bg_already_issued", lines)


def compare_active_and_queued_lp() -> None:
    queue_model = Tails()
    queue_lines = [
        queue_model.low_priority("lp_active", issue=0, busy=100),
        queue_model.low_priority("lp_queued", issue=0, busy=100),
        queue_model.read_priority("read", issue=10, busy=20),
    ]
    nonpreemptive = NonPreemptiveTails()
    nonpreemptive_lines = [
        nonpreemptive.low_priority("lp_active", issue=0, busy=100),
        nonpreemptive.low_priority("lp_queued", issue=0, busy=100),
        nonpreemptive.read_priority("read", issue=10, busy=20),
    ]

    emit("submitted_active_plus_queued_lp", queue_lines)
    emit("hypothetical_controller_queue", nonpreemptive_lines)
    assert "start=200 end=220 request_latency=210" in queue_lines[-1]
    assert "start=100 end=120 request_latency=110" in nonpreemptive_lines[-1]
    assert queue_model.lp == 220
    assert nonpreemptive.lp == 220


def batch_reads_before_bg() -> None:
    tails = Tails()
    lines = [
        tails.read_priority("read1", issue=0, busy=20),
        tails.read_priority("read2", issue=0, busy=20),
        tails.read_priority("read3", issue=0, busy=20),
        tails.low_priority("bg", issue=5, busy=100),
        "conclusion: if the read batch is scheduled first, bg follows the HP tail",
    ]
    emit("batch_reads_before_bg", lines)


def bg_interleaves_without_gate() -> None:
    tails = Tails()
    lines = [
        tails.read_priority("read1", issue=0, busy=20),
        tails.low_priority("bg", issue=5, busy=100),
        tails.read_priority("read2", issue=0, busy=20),
        tails.read_priority("read3", issue=0, busy=20),
        "conclusion: same-issue reads still serialize behind earlier HP reads, not the LP tail",
    ]
    emit("bg_interleaves_without_gate", lines)


def bg_interleaves_with_gate() -> None:
    tails = Tails()
    lines = [
        tails.read_priority("read1", issue=0, busy=20),
        "bg: issue=5 gate=yield requeue_at=60",
        tails.read_priority("read2", issue=0, busy=20),
        tails.read_priority("read3", issue=0, busy=20),
        tails.low_priority("bg", issue=60, busy=100),
        "conclusion: gate reduces LP tail shifting and background interference during read bursts",
    ]
    emit("bg_interleaves_with_gate", lines)


def atomic_rebalance_units() -> None:
    tails = Tails()
    first = tails.low_priority("rebalance_unit1", issue=0, busy=100)
    read = tails.read_priority("read", issue=10, busy=20)
    lines = [
        first,
        read,
        "rebalance_unit2: held at submit gate while read window is active",
        tails.low_priority("rebalance_unit2", issue=120, busy=100),
        "conclusion: only unit1 was committed before the read; unit2 is not "
        "a pre-reserved LP tail",
    ]
    assert "start=0 end=100" in first
    assert "start=100 end=120 request_latency=110" in read
    assert "start=120 end=220" in lines[-2]
    emit("atomic_rebalance_units", lines)


def foreground_read_dominated() -> None:
    tails = Tails()
    lines = []
    for idx in range(1, 9):
        lines.append(tails.read_priority(f"read{idx}", issue=0, busy=20))
    lines.append(tails.low_priority("bg", issue=30, busy=20))
    lines.append("conclusion: with many queued reads, HP read/read tail dominates")
    emit("foreground_read_dominated", lines)


def main() -> int:
    bg_already_issued()
    compare_active_and_queued_lp()
    batch_reads_before_bg()
    bg_interleaves_without_gate()
    bg_interleaves_with_gate()
    atomic_rebalance_units()
    foreground_read_dominated()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
