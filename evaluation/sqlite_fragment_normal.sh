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
SQLITE_TARGET_BYTES=${SQLITE_TARGET_BYTES:-10G}
SQLITE_TRACE_DIR=${SQLITE_TRACE_DIR:-}

SQLITE_TABLE_COUNT=${SQLITE_TABLE_COUNT:-100}
SQLITE_INTERLEAVE_ROWS=${SQLITE_INTERLEAVE_ROWS:-1000}
SQLITE_INTERLEAVE_READS=${SQLITE_INTERLEAVE_READS:-1000}

mkdir -p "$RESULT_FOLDER"
mkdir -p "$TARGET_FOLDER"

drop_caches() {
    sync
    echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
}

run_normal_suite() {
    local tag="fragment_on"
    local trace_dir="${SQLITE_TRACE_DIR:-${TARGET_FOLDER%/}/sqlite_traces}"
    local trace_base="${trace_dir%/}"
    local trace_normal="${trace_base}/sqlite_trace_${tag}_normal.trace"
    local layout_meta="${TARGET_FOLDER%/}/sqlite_layout_${tag}.meta"

    ./nvmevstart_on.sh
    sleep 1
    lsblk
    source setdevice.sh
    mkdir -p "$trace_dir"

    drop_caches
    numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./sqlite_append --mode init \
        --target-bytes "$SQLITE_TARGET_BYTES" \
        --table-count "$SQLITE_TABLE_COUNT" \
        --interleave-rows "$SQLITE_INTERLEAVE_ROWS" \
        --interleave-reads "$SQLITE_INTERLEAVE_READS" \
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

    source resetdevice.sh
}

./disablemeta.sh
run_normal_suite
./enablemeta.sh
