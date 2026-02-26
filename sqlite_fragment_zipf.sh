#!/bin/bash -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

source commonvariables.sh

SRC_PATH="sqlite"

if [[ ! -f ./sqlite_append ]] || [[ $FORCE_REBUILD == 1 ]]; then
    gcc -D_GNU_SOURCE -D TARGET_FOLDER="\"$TARGET_FOLDER\"" -D RESULT_FOLDER="\"$RESULT_FOLDER\"" -o ./sqlite_append ./$SRC_PATH/sqlite_append.c -lsqlite3 -lm
fi

ZIPF_ALPHA=${ZIPF_ALPHA:-0.8}
ZIPF_SEED=${ZIPF_SEED:-42}
EXP_LAMBDA=${EXP_LAMBDA:-0.0000005}
EXP_SEED=${EXP_SEED:-4242}
NORMAL_MEAN=${NORMAL_MEAN:-5000}
NORMAL_STDDEV=${NORMAL_STDDEV:-800}
NORMAL_SEED=${NORMAL_SEED:-314159}
SQLITE_TARGET_BYTES=${SQLITE_TARGET_BYTES:-10G}
SQLITE_TRACE_DIR=${SQLITE_TRACE_DIR:-}

SQLITE_TABLE_COUNT=${SQLITE_TABLE_COUNT:-100}
SQLITE_ROWS_PER_TABLE=${SQLITE_ROWS_PER_TABLE:-400}
SQLITE_INTERLEAVE_ROWS=${SQLITE_INTERLEAVE_ROWS:-1000}
SQLITE_INTERLEAVE_READS=${SQLITE_INTERLEAVE_READS:-1000}
SQLITE_STRICT_COLD_PER_SELECT=${SQLITE_STRICT_COLD_PER_SELECT:-1}
SQLITE_PAGE_TIER_PATH=${SQLITE_PAGE_TIER_PATH:-/sys/kernel/debug/nvmev/ftl0/page_tier}
SQLITE_ACCESS_COUNT_PATH=${SQLITE_ACCESS_COUNT_PATH:-/sys/kernel/debug/nvmev/ftl0/access_count}
SQLITE_FTL_HOST_PAGE_BYTES=${SQLITE_FTL_HOST_PAGE_BYTES:-4K}
SQLITE_DIRECT_IO=${SQLITE_DIRECT_IO:-1}

mkdir -p "$RESULT_FOLDER"
mkdir -p "$TARGET_FOLDER"

drop_caches() {
    sync
    echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
}

run_zipf_suite() {
    local tag="fragment_on"
    local trace_dir="${SQLITE_TRACE_DIR:-${RESULT_FOLDER%/}/sqlite_traces}"
    local trace_base="${trace_dir%/}"
    local trace_zipf="${trace_base}/sqlite_trace_${tag}_zipf.trace"
    local layout_meta="${RESULT_FOLDER%/}/sqlite_layout_${tag}.meta"
    local strict_args=()

    if [[ "$SQLITE_STRICT_COLD_PER_SELECT" == "1" ]]; then
        strict_args+=(--strict-cold-per-select)
    fi
    if [[ "$SQLITE_DIRECT_IO" == "1" ]]; then
        strict_args+=(--direct-io)
    fi

    ./nvmevstart_on.sh
    sleep 1
    lsblk
    source setdevice.sh
    mkdir -p "$trace_dir"

    drop_caches
    numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./sqlite_append --mode init \
        --target-bytes "$SQLITE_TARGET_BYTES" \
        --table-count "$SQLITE_TABLE_COUNT" \
        --rows-per-table "$SQLITE_ROWS_PER_TABLE" \
        --interleave-rows "$SQLITE_INTERLEAVE_ROWS" \
        --interleave-reads "$SQLITE_INTERLEAVE_READS" \
        --page-tier-path "$SQLITE_PAGE_TIER_PATH" \
        --access-count-path "$SQLITE_ACCESS_COUNT_PATH" \
        --ftl-host-page-bytes "$SQLITE_FTL_HOST_PAGE_BYTES" \
        --distribution zipf \
        --zipf-seed "$ZIPF_SEED" \
        --exp-seed "$EXP_SEED" \
        --normal-seed "$NORMAL_SEED" \
        --alpha "$ZIPF_ALPHA" \
        --lambda "$EXP_LAMBDA" \
        --normal-mean "$NORMAL_MEAN" \
        --normal-stddev "$NORMAL_STDDEV" \
        --tag "$tag" \
        --trace-dir "$trace_dir" \
        "${strict_args[@]}" \
        >"./$RESULT_FOLDER/sqlite_${tag}_init.txt"

    if [[ ! -f "$trace_zipf" ]]; then
        echo "Trace file not found: $trace_zipf" >&2
        source resetdevice.sh
        exit 1
    fi
    if [[ ! -f "$layout_meta" ]]; then
        echo "Layout file not found: $layout_meta" >&2
        source resetdevice.sh
        exit 1
    fi

    source resetdevice.sh
}

./disablemeta.sh
run_zipf_suite
./enablemeta.sh
