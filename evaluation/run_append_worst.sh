#!/bin/bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$script_dir"

target_label="Append Worst without Approach"

for i in {1..10}; do
    ./hypothetical_test1.sh

    line=$(./printresult.sh | grep -F "$target_label" | head -n 1 || true)
    if [[ -z "$line" ]]; then
        echo "第${i}次执行未找到 ${target_label} 行" >&2
        exit 1
    fi
    echo "$line"
done
