#!/bin/bash -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

source commonvariables.sh

SRC_PATH="sqlite"

if [[ ! -f ./sqlite_append ]] || [[ $FORCE_REBUILD == 1 ]]; then
    gcc -D TARGET_FOLDER="\"$TARGET_FOLDER\"" -o ./sqlite_append ./$SRC_PATH/sqlite_append.c -lsqlite3 -lm
fi

READ_QUERIES=${READ_QUERIES:-1500000}
ZIPF_ALPHA=${ZIPF_ALPHA:-1.2}
ZIPF_SEED=${ZIPF_SEED:-42}
EXP_LAMBDA=${EXP_LAMBDA:-0.0008}
EXP_SEED=${EXP_SEED:-4242}
NORMAL_MEAN=${NORMAL_MEAN:--1}
NORMAL_STDDEV=${NORMAL_STDDEV:-150000}
NORMAL_SEED=${NORMAL_SEED:-314159}
SQLITE_TARGET_BYTES=${SQLITE_TARGET_BYTES:-6G}
SQLITE_TRACE_DIR=${SQLITE_TRACE_DIR:-}
SCAN_ITERS=${SCAN_ITERS:-10}

ACCESS_COUNT_PATH=${ACCESS_COUNT_PATH:-/sys/kernel/debug/nvmev/ftl0/access_count}
ACCESS_INJECT_PATH=${ACCESS_INJECT_PATH:-/sys/kernel/debug/nvmev/ftl0/access_inject}

mkdir -p "$RESULT_FOLDER"

drop_caches() {
    sync
    echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
}

ensure_access_inject() {
    local debugfs_mount
    debugfs_mount=$(mount | grep -w debugfs || true)
    if [[ -z $debugfs_mount ]]; then
        sudo mount -t debugfs debugfs /sys/kernel/debug
    fi

    local inject_path="$ACCESS_INJECT_PATH"
    local count_path="$ACCESS_COUNT_PATH"
    local attempts=40
    while (( attempts > 0 )); do
        if [[ -w $inject_path && -r $count_path ]]; then
            return 0
        fi
        sleep 0.25
        attempts=$((attempts - 1))
    done

    echo "access inject/count nodes not ready under /sys/kernel/debug" >&2
    exit 1
}

run_cold_scan() {
    local tag=$1
    local dist_suffix=$2
    local scan_log="./$RESULT_FOLDER/sqlite_${tag}_${dist_suffix}_scan.txt"

    : >"$scan_log"
    for ((iter = 0; iter < SCAN_ITERS; ++iter)); do
        drop_caches
        output=$(numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./sqlite_append \
            --mode scan --scan-iters 1 \
            --tag "$tag")
        printf "%s\n" "$output" >>"$scan_log"
    done

    python3 - "$scan_log" <<'PY' >>"$scan_log"
import re, statistics, sys
log_path = sys.argv[1]
times = []
throughputs = []
with open(log_path) as fh:
    for line in fh:
        if not line.startswith("[sqlite_scan] iter="):
            continue
        mt = re.search(r"time=([0-9.]+)s", line)
        if mt:
            times.append(float(mt.group(1)))
        mp = re.search(r"throughput=([0-9.]+)MB/s", line)
        if mp:
            throughputs.append(float(mp.group(1)))
if not times:
    print("[sqlite_scan_cold] no data captured")
    sys.exit(0)
avg_time = statistics.mean(times)
avg_tp = statistics.mean(throughputs) if throughputs else 0.0
print(f"[sqlite_scan_cold] average_time={avg_time:.6f}s average_throughput={avg_tp:.2f}MB/s ({len(times)} cold iterations)")
PY
}

run_normal_suite() {
    local tag="fragment_on"
    local dist_suffix="normal"
    local trace_dir="${SQLITE_TRACE_DIR:-${TARGET_FOLDER%/}/sqlite_traces}"
    local trace_base="${trace_dir%/}"
    local trace_normal="${trace_base}/sqlite_trace_${tag}_normal.trace"
    local layout_meta="${TARGET_FOLDER%/}/sqlite_layout_${tag}.meta"

    ./nvmevstart_on.sh
    sleep 1
    lsblk
    source setdevice.sh
    ensure_access_inject
    mkdir -p "$trace_dir"

    drop_caches
    numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./sqlite_append --mode init \
        --target-bytes "$SQLITE_TARGET_BYTES" \
        --zipf-seed "$ZIPF_SEED" \
        --exp-seed "$EXP_SEED" \
        --normal-seed "$NORMAL_SEED" \
        --alpha "$ZIPF_ALPHA" \
        --lambda "$EXP_LAMBDA" \
        --normal-mean "$NORMAL_MEAN" \
        --normal-stddev "$NORMAL_STDDEV" \
        --tag "$tag" \
        --trace-dir "$trace_dir" \
        >"./$RESULT_FOLDER/sqlite_${tag}_init.txt"

    if [[ ! -f "$trace_normal" ]]; then
        echo "Trace file not found: $trace_normal" >&2
        source resetdevice.sh
        exit 1
    fi
    if [[ ! -f "$layout_meta" ]]; then
        echo "Layout file not found: $layout_meta" >&2
        source resetdevice.sh
        exit 1
    fi

    local total_rows
    total_rows=$(grep -m1 '^total_rows=' "$layout_meta" | cut -d'=' -f2)
    if [[ -z $total_rows ]]; then
        echo "Failed to parse total_rows from $layout_meta" >&2
        source resetdevice.sh
        exit 1
    fi

    drop_caches
    numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./sqlite_append \
        --mode read --distribution normal --reads "$READ_QUERIES" \
        --normal-mean "$NORMAL_MEAN" --normal-stddev "$NORMAL_STDDEV" \
        --seed "$NORMAL_SEED" \
        --tag "$tag" \
        --trace-mode replay --trace-path "$trace_normal" \
        --log "./$RESULT_FOLDER/sqlite_${tag}_normal_trace.csv" \
        --heatmap "./$RESULT_FOLDER/sqlite_${tag}_normal_heat.csv" \
        --human-log \
        >"./$RESULT_FOLDER/sqlite_${tag}_normal.txt"

    drop_caches
    numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./sqlite_append \
        --mode read --distribution sequential --reads "$total_rows" \
        --tag "$tag" \
        --log "./$RESULT_FOLDER/sqlite_${tag}_${dist_suffix}_fullcover_trace.csv" \
        --heatmap "./$RESULT_FOLDER/sqlite_${tag}_${dist_suffix}_fullcover_heat.csv" \
        >"./$RESULT_FOLDER/sqlite_${tag}_${dist_suffix}_fullcover.txt"

    run_cold_scan "$tag" "$dist_suffix"

    source resetdevice.sh
}

./disablemeta.sh
run_normal_suite
./enablemeta.sh
