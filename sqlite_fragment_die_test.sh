#!/bin/bash -e
#
# sqlite_fragment_die_test.sh
#
# Die-contention test: compares die_all (die affinity ON) vs die_no2 (die affinity OFF)
# with GC NAND timing enabled, using concurrent random cold reads.
#
# Prerequisites:
#   1. Run  cd nvmevirt_DA && ./build_die.sh die_all && ./build_die.sh die_no2
#      to produce nvmev_die_all.ko and nvmev_die_no2.ko
#   2. Ensure sqlite3 dev headers are installed.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

source commonvariables.sh

# ---------- tunables ----------
THREAD_COUNTS="${THREAD_COUNTS:-4 8 16}"
COLD_READS_PER_TBL="${COLD_READS_PER_TBL:-5000}"
VARIANTS="${VARIANTS:-die_all die_no2}"

SQLITE_TARGET_BYTES=${SQLITE_TARGET_BYTES:-10G}
SQLITE_TABLE_COUNT=${SQLITE_TABLE_COUNT:-100}
SQLITE_ROWS_PER_TABLE=${SQLITE_ROWS_PER_TABLE:-400}
SQLITE_INTERLEAVE_ROWS=${SQLITE_INTERLEAVE_ROWS:-1000}
SQLITE_INTERLEAVE_READS=${SQLITE_INTERLEAVE_READS:-1000}
SQLITE_PAGE_TIER_PATH=${SQLITE_PAGE_TIER_PATH:-/sys/kernel/debug/nvmev/ftl0/page_tier}
SQLITE_ACCESS_COUNT_PATH=${SQLITE_ACCESS_COUNT_PATH:-/sys/kernel/debug/nvmev/ftl0/access_count}
SQLITE_FTL_HOST_PAGE_BYTES=${SQLITE_FTL_HOST_PAGE_BYTES:-4K}
SQLITE_DIRECT_IO=${SQLITE_DIRECT_IO:-1}
SQLITE_FAST_INIT_PROFILE=${SQLITE_FAST_INIT_PROFILE:-1}
NORMAL_MEAN=${NORMAL_MEAN:--1}
NORMAL_STDDEV=${NORMAL_STDDEV:-400}
NORMAL_SEED=${NORMAL_SEED:-314159}
ZIPF_SEED=${ZIPF_SEED:-42}
EXP_SEED=${EXP_SEED:-4242}
ZIPF_ALPHA=${ZIPF_ALPHA:-1.2}
EXP_LAMBDA=${EXP_LAMBDA:-0.0008}
# ---------- end tunables ----------

SRC_PATH="sqlite"
NVMEV_DIR="${SCRIPT_DIR}/../nvmevirt_DA"
DIE_RESULT_BASE="${RESULT_FOLDER%/}/die_test"

# Compile sqlite_append_die (with pthread)
if [[ ! -f ./sqlite_append_die ]] || [[ $FORCE_REBUILD == 1 ]]; then
    echo "=== Compiling sqlite_append_die ==="
    gcc -D_GNU_SOURCE \
        -DTARGET_FOLDER="\"$TARGET_FOLDER\"" \
        -DRESULT_FOLDER="\"$RESULT_FOLDER\"" \
        -o ./sqlite_append_die \
        ./$SRC_PATH/sqlite_append_die.c \
        -lsqlite3 -lm -lpthread
fi

drop_caches() {
    sync
    echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
}

load_die_module() {
    local variant="$1"
    local ko_path="${NVMEV_DIR}/nvmev_${variant}.ko"
    if [[ ! -f "$ko_path" ]]; then
        echo "ERROR: $ko_path not found. Run build_die.sh first." >&2
        exit 1
    fi
    echo "=== Loading $ko_path ==="
    sudo insmod "$ko_path" memmap_start=256G memmap_size=64G cpus=131,132,135,136
    sleep 1
}

unload_module() {
    sudo rmmod nvmev 2>/dev/null || true
    sleep 1
}

run_one_test() {
    local variant="$1"
    local threads="$2"
    local tag="die_${variant}_t${threads}"
    local out_dir="${DIE_RESULT_BASE}/${variant}/t${threads}"

    mkdir -p "$out_dir"

    echo ""
    echo "================================================================"
    echo "  variant=$variant  threads=$threads  tag=$tag"
    echo "================================================================"

    unload_module
    load_die_module "$variant"

    lsblk
    source setdevice.sh

    echo 0 | sudo tee /sys/block/${DATA_NAME}/queue/read_ahead_kb >/dev/null 2>&1 || true
    drop_caches

    mkdir -p "$TARGET_FOLDER"

    local extra_args=()
    if [[ "$SQLITE_DIRECT_IO" == "1" ]]; then
        extra_args+=(--direct-io)
    fi
    if [[ "$SQLITE_FAST_INIT_PROFILE" == "1" ]]; then
        extra_args+=(--fast-init-profile)
    fi

    numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./sqlite_append_die --mode init \
        --target-bytes "$SQLITE_TARGET_BYTES" \
        --table-count "$SQLITE_TABLE_COUNT" \
        --rows-per-table "$SQLITE_ROWS_PER_TABLE" \
        --interleave-rows "$SQLITE_INTERLEAVE_ROWS" \
        --interleave-reads "$SQLITE_INTERLEAVE_READS" \
        --page-tier-path "$SQLITE_PAGE_TIER_PATH" \
        --access-count-path "$SQLITE_ACCESS_COUNT_PATH" \
        --ftl-host-page-bytes "$SQLITE_FTL_HOST_PAGE_BYTES" \
        --distribution normal \
        --zipf-seed "$ZIPF_SEED" \
        --exp-seed "$EXP_SEED" \
        --normal-seed "$NORMAL_SEED" \
        --alpha "$ZIPF_ALPHA" \
        --lambda "$EXP_LAMBDA" \
        --normal-mean "$NORMAL_MEAN" \
        --normal-stddev "$NORMAL_STDDEV" \
        --cold-full-read-mode random-concurrent \
        --cold-concurrent-threads "$threads" \
        --cold-random-reads-per-tbl "$COLD_READS_PER_TBL" \
        --cold-disable-read-repromotion \
        --strict-cold-per-select \
        "${extra_args[@]}" \
        --tag "$tag" \
        >"${out_dir}/sqlite_${tag}_init.txt" 2>&1

    # Copy result CSVs into the per-test output directory
    cp "${RESULT_FOLDER}"/sqlite_table_tier_${tag}.csv "${out_dir}/" 2>/dev/null || true
    cp "${RESULT_FOLDER}"/sqlite_table_${tag}.csv      "${out_dir}/" 2>/dev/null || true
    cp "${RESULT_FOLDER}"/sqlite_row_${tag}.csv         "${out_dir}/" 2>/dev/null || true

    echo "=== Done: variant=$variant threads=$threads ==="
    echo "  Output: ${out_dir}/sqlite_${tag}_init.txt"

    source resetdevice.sh
}

# ---------- main ----------
./disablemeta.sh

mkdir -p "$DIE_RESULT_BASE"

for variant in $VARIANTS; do
    for threads in $THREAD_COUNTS; do
        run_one_test "$variant" "$threads"
    done
done

./enablemeta.sh

echo ""
echo "========================================"
echo "  All die-contention tests completed."
echo "  Results in: $DIE_RESULT_BASE"
echo "========================================"
