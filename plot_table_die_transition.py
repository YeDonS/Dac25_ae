#!/usr/bin/env python3

import argparse
import csv
from html import escape
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(
        description="Plot one table's initial-die to final-die transition heatmap as SVG."
    )
    parser.add_argument("--input", required=True, help="sqlite_table_die_transition_*.csv path")
    parser.add_argument("--table-id", type=int, help="target table_id")
    parser.add_argument(
        "--table-ids",
        help="comma-separated table_id list; writes one SVG per table into --output-dir",
    )
    parser.add_argument("--table-name", help="target table_name")
    parser.add_argument(
        "--all-tables",
        action="store_true",
        help="render every table found in the CSV into --output-dir",
    )
    parser.add_argument(
        "--metric",
        choices=("count", "ratio"),
        default="count",
        help="Cell value to plot",
    )
    parser.add_argument(
        "--include-unknown",
        action="store_true",
        help="Keep the initial_die=-1 row in the plot",
    )
    parser.add_argument("--output", help="output SVG path for a single table")
    parser.add_argument(
        "--output-dir",
        help="output directory for --table-ids or --all-tables; one SVG per table",
    )
    args = parser.parse_args()

    selector_count = 0
    if args.table_id is not None:
        selector_count += 1
    if args.table_ids:
        selector_count += 1
    if args.table_name:
        selector_count += 1
    if args.all_tables:
        selector_count += 1
    if selector_count != 1:
        parser.error("choose exactly one of --table-id, --table-ids, --table-name, or --all-tables")

    if args.table_ids or args.all_tables:
        if not args.output_dir:
            parser.error("--output-dir is required for --table-ids or --all-tables")
        if args.output:
            parser.error("--output cannot be used with --table-ids or --all-tables")
    else:
        if not args.output:
            parser.error("--output is required for single-table mode")
        if args.output_dir:
            parser.error("--output-dir is only for --table-ids or --all-tables")

    return args


def load_rows(path):
    rows = []
    with open(path, newline="") as fp:
        reader = csv.DictReader(fp)
        for row in reader:
            rows.append(
                {
                    "table_id": int(row["table_id"]),
                    "table_name": row["table_name"],
                    "initial_die": int(row["initial_die"]),
                    "final_die": int(row["final_die"]),
                    "count": int(row["page_count"]),
                    "ratio": float(row["page_ratio"]),
                }
            )
    return rows


def group_rows(all_rows):
    grouped = {}
    for row in all_rows:
        key = (row["table_id"], row["table_name"])
        grouped.setdefault(key, []).append(row)
    return grouped


def build_matrix(rows, metric, include_unknown):
    final_dies = sorted({row["final_die"] for row in rows})
    initial_dies = sorted({row["initial_die"] for row in rows})

    if not include_unknown:
        initial_dies = [die for die in initial_dies if die >= 0]
    if not initial_dies:
        raise SystemExit("no known initial_die rows found; rerun with --include-unknown")

    initial_index = {die: idx for idx, die in enumerate(initial_dies)}
    final_index = {die: idx for idx, die in enumerate(final_dies)}
    matrix = [[0.0 for _ in final_dies] for _ in initial_dies]

    for row in rows:
        initial_die = row["initial_die"]
        if initial_die not in initial_index:
            continue
        matrix[initial_index[initial_die]][final_index[row["final_die"]]] = row[metric]

    return initial_dies, final_dies, matrix


def interpolate_color(value, max_value):
    if max_value <= 0.0:
        return "#f8fbff"
    ratio = max(0.0, min(1.0, value / max_value))
    start = (248, 251, 255)
    end = (8, 81, 156)
    rgb = []
    for idx in range(3):
        channel = round(start[idx] + (end[idx] - start[idx]) * ratio)
        rgb.append(channel)
    return "#{:02x}{:02x}{:02x}".format(*rgb)


def text_color(value, max_value):
    if max_value <= 0.0:
        return "#111111"
    return "#ffffff" if value >= max_value * 0.55 else "#111111"


def build_svg(rows, initial_dies, final_dies, matrix, metric):
    title_name = rows[0]["table_name"]
    title_id = rows[0]["table_id"]
    max_value = max((value for row in matrix for value in row), default=0.0)

    cell_w = 88
    cell_h = 56
    left_pad = 110
    top_pad = 90
    right_pad = 30
    bottom_pad = 70
    width = left_pad + cell_w * len(final_dies) + right_pad
    height = top_pad + cell_h * len(initial_dies) + bottom_pad

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        f'<text x="{width / 2:.1f}" y="34" text-anchor="middle" '
        'font-family="Helvetica, Arial, sans-serif" font-size="18" fill="#111111">'
        f'{escape(f"Table {title_id} ({title_name}) die transition")}</text>',
        f'<text x="{width / 2:.1f}" y="58" text-anchor="middle" '
        'font-family="Helvetica, Arial, sans-serif" font-size="12" fill="#555555">'
        f'{escape(f"metric={metric}")}</text>',
        f'<text x="{left_pad + (cell_w * len(final_dies)) / 2:.1f}" y="{height - 20}" '
        'text-anchor="middle" font-family="Helvetica, Arial, sans-serif" font-size="13" '
        'fill="#111111">Final die</text>',
        f'<text x="28" y="{top_pad + (cell_h * len(initial_dies)) / 2:.1f}" '
        'text-anchor="middle" font-family="Helvetica, Arial, sans-serif" font-size="13" '
        'fill="#111111" transform="rotate(-90 28 '
        f'{top_pad + (cell_h * len(initial_dies)) / 2:.1f})">Initial die</text>',
    ]

    for idx, die in enumerate(final_dies):
        x = left_pad + idx * cell_w + cell_w / 2
        parts.append(
            f'<text x="{x:.1f}" y="{top_pad - 14}" text-anchor="middle" '
            'font-family="Helvetica, Arial, sans-serif" font-size="12" fill="#111111">'
            f"{escape(str(die))}</text>"
        )

    for idx, die in enumerate(initial_dies):
        y = top_pad + idx * cell_h + cell_h / 2 + 4
        label = "unknown" if die < 0 else str(die)
        parts.append(
            f'<text x="{left_pad - 12}" y="{y:.1f}" text-anchor="end" '
            'font-family="Helvetica, Arial, sans-serif" font-size="12" fill="#111111">'
            f"{escape(label)}</text>"
        )

    for y_idx, row in enumerate(matrix):
        for x_idx, value in enumerate(row):
            x = left_pad + x_idx * cell_w
            y = top_pad + y_idx * cell_h
            fill = interpolate_color(value, max_value)
            label = f"{value:.3f}" if metric == "ratio" else str(int(value))
            parts.append(
                f'<rect x="{x}" y="{y}" width="{cell_w}" height="{cell_h}" '
                f'fill="{fill}" stroke="#d0d7de" stroke-width="1"/>'
            )
            if value != 0:
                parts.append(
                    f'<text x="{x + cell_w / 2:.1f}" y="{y + cell_h / 2 + 4:.1f}" '
                    'text-anchor="middle" font-family="Helvetica, Arial, sans-serif" '
                    f'font-size="12" fill="{text_color(value, max_value)}">{escape(label)}</text>'
                )

    parts.append("</svg>")
    return "\n".join(parts)


def main():
    args = parse_args()
    all_rows = load_rows(args.input)
    grouped = group_rows(all_rows)

    if not grouped:
        raise SystemExit("no rows found in input CSV")

    selected = []
    if args.table_id is not None:
        for key, rows in grouped.items():
            if key[0] == args.table_id:
                selected.append((key, rows))
                break
    elif args.table_name:
        for key, rows in grouped.items():
            if key[1] == args.table_name:
                selected.append((key, rows))
                break
    elif args.table_ids:
        requested = []
        seen = set()
        for part in args.table_ids.split(","):
            part = part.strip()
            if not part:
                continue
            table_id = int(part)
            if table_id not in seen:
                seen.add(table_id)
                requested.append(table_id)
        for table_id in requested:
            for key, rows in grouped.items():
                if key[0] == table_id:
                    selected.append((key, rows))
                    break
    else:
        selected = sorted(grouped.items(), key=lambda item: item[0][0])

    if not selected:
        raise SystemExit("no matching rows found")

    if args.output:
        _, rows = selected[0]
        initial_dies, final_dies, matrix = build_matrix(
            rows, args.metric, args.include_unknown
        )
        svg = build_svg(rows, initial_dies, final_dies, matrix, args.metric)

        output = Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(svg, encoding="utf-8")
        return

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    for (table_id, table_name), rows in selected:
        initial_dies, final_dies, matrix = build_matrix(
            rows, args.metric, args.include_unknown
        )
        svg = build_svg(rows, initial_dies, final_dies, matrix, args.metric)
        output = output_dir / f"tbl_{table_id:02d}_{table_name}_{args.metric}.svg"
        output.write_text(svg, encoding="utf-8")


if __name__ == "__main__":
    main()
