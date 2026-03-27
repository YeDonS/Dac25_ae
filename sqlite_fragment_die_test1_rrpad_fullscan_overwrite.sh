#!/bin/bash -e
#
# sqlite_fragment_die_test1_rrpad_fullscan_overwrite.sh
#
# Die-affinity test 1 (RRPAD + FULL SCAN + OVERWRITE):
#   - init uses sqlite_append_die_affinity_rrpad.c
#   - cold read phase enables test_phase instrumentation
#   - before each cold table scan, overwrite 10% of rows by default
#   - all threads then scan the same table together before moving on
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

source commonvariables.sh

# ---------- tunables ----------
THREAD_COUNTS="${THREAD_COUNTS:-1}"
VARIANTS="${VARIANTS:-die_base die_no1 die_no2 die_no3 die_all}"

SQLITE_TARGET_BYTES=${SQLITE_TARGET_BYTES:-10G}
SQLITE_TABLE_COUNT=${SQLITE_TABLE_COUNT:-100}
SQLITE_ROWS_PER_TABLE=${SQLITE_ROWS_PER_TABLE:-3200}
SQLITE_INTERLEAVE_ROWS=${SQLITE_INTERLEAVE_ROWS:-8000}
SQLITE_INTERLEAVE_READS=${SQLITE_INTERLEAVE_READS:-8000}
SQLITE_PAGE_TIER_PATH=${SQLITE_PAGE_TIER_PATH:-/sys/kernel/debug/nvmev/ftl0/page_tier}
SQLITE_ACCESS_COUNT_PATH=${SQLITE_ACCESS_COUNT_PATH:-/sys/kernel/debug/nvmev/ftl0/access_count}
SQLITE_DIE_AFFINITY_STATS_PATH=${SQLITE_DIE_AFFINITY_STATS_PATH:-/sys/kernel/debug/nvmev/ftl0/die_affinity_stats}
SQLITE_TEST_PHASE_PATH=${SQLITE_TEST_PHASE_PATH:-/sys/kernel/debug/nvmev/ftl0/test_phase}
SQLITE_TEST_PHASE_STATS_PATH=${SQLITE_TEST_PHASE_STATS_PATH:-/sys/kernel/debug/nvmev/ftl0/test_phase_stats}
SQLITE_FTL_HOST_PAGE_BYTES=${SQLITE_FTL_HOST_PAGE_BYTES:-4K}
SQLITE_DIRECT_IO=${SQLITE_DIRECT_IO:-1}
SQLITE_FAST_INIT_PROFILE=${SQLITE_FAST_INIT_PROFILE:-1}
SQLITE_COLD_FULL_READ_ITERS=${SQLITE_COLD_FULL_READ_ITERS:-1}
SQLITE_COLD_FULL_READ_MODE=${SQLITE_COLD_FULL_READ_MODE:-full-scan-overwrite-concurrent}
SQLITE_COLD_OVERWRITE_PCT=${SQLITE_COLD_OVERWRITE_PCT:-10}
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
DIE_RESULT_BASE="${RESULT_FOLDER%/}/die_test1_natural_rrpad_fullscan_overwrite"

EXE_NAME="sqlite_append_die_affinity_rrpad"
SRC_FILE="./$SRC_PATH/sqlite_append_die_affinity_rrpad.c"

if [[ ! -f ./${EXE_NAME} ]] || [[ $FORCE_REBUILD == 1 ]]; then
    echo "=== Compiling ${EXE_NAME} (32KB rows, rrpad overwrite full-scan test) ==="
    gcc -D_GNU_SOURCE \
        -DTARGET_FOLDER="\"$TARGET_FOLDER\"" \
        -DRESULT_FOLDER="\"$RESULT_FOLDER\"" \
        -o ./${EXE_NAME} \
        "${SRC_FILE}" \
        -lsqlite3 -lm -lpthread
fi

mkdir -p "$RESULT_FOLDER"
mkdir -p "$TARGET_FOLDER"

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
    echo "=== Loading $ko_path (via nvmevstart_on.sh) ==="
    if [[ -f ./nvmev_on.ko ]]; then
        cp ./nvmev_on.ko ./nvmev_on.ko.die_bak
    fi
    cp "$ko_path" ./nvmev_on.ko
    ./nvmevstart_on.sh
    if [[ -f ./nvmev_on.ko.die_bak ]]; then
        mv ./nvmev_on.ko.die_bak ./nvmev_on.ko
    fi
    sleep 1
}

run_one_test() {
    local variant="$1"
    local threads="$2"
    local tag="die_natural_rrpad_fullscan_overwrite_${variant}_t${threads}"
    local init_txt="${RESULT_FOLDER%/}/sqlite_die_natural_rrpad_fullscan_overwrite_init_${variant}_t${threads}.txt"
    local out_dir="${DIE_RESULT_BASE}/${variant}/t${threads}"

    mkdir -p "$out_dir"

    echo ""
    echo "================================================================"
    echo "  [TEST1-NATURAL-RRPAD-FULLSCAN-OVERWRITE] variant=$variant  threads=$threads  tag=$tag"
    echo "  row_bytes=32KB  tables=$SQLITE_TABLE_COUNT  rows/tbl=$SQLITE_ROWS_PER_TABLE"
    echo "  target=$SQLITE_TARGET_BYTES  phase_pad=ON  cold_mode=$SQLITE_COLD_FULL_READ_MODE"
    echo "  cold_overwrite_pct=$SQLITE_COLD_OVERWRITE_PCT"
    echo "================================================================"

    load_die_module "$variant"

    lsblk
    source setdevice.sh

    echo 0 | sudo tee /sys/block/${DATA_NAME}/queue/read_ahead_kb >/dev/null 2>&1 || true
    echo "[readahead] set /sys/block/${DATA_NAME}/queue/read_ahead_kb = $(cat /sys/block/${DATA_NAME}/queue/read_ahead_kb 2>/dev/null || echo N/A)"
    drop_caches

    mkdir -p "$TARGET_FOLDER"

    local extra_args=()
    if [[ "$SQLITE_DIRECT_IO" == "1" ]]; then
        extra_args+=(--direct-io)
    fi
    if [[ "$SQLITE_FAST_INIT_PROFILE" == "1" ]]; then
        extra_args+=(--fast-init-profile)
    fi

    numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./${EXE_NAME} --mode init \
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
        --cold-full-read-mode "$SQLITE_COLD_FULL_READ_MODE" \
        --cold-full-read-iters "$SQLITE_COLD_FULL_READ_ITERS" \
        --cold-concurrent-threads "$threads" \
        --cold-overwrite-pct "$SQLITE_COLD_OVERWRITE_PCT" \
        --test-phase-path "$SQLITE_TEST_PHASE_PATH" \
        --strict-cold-per-select \
        "${extra_args[@]}" \
        --tag "$tag" \
        >"$init_txt" 2>&1

    cp "$init_txt" "${out_dir}/" 2>/dev/null || true
    cp "${RESULT_FOLDER}"/sqlite_table_tier_${tag}.csv "${out_dir}/" 2>/dev/null || true
    cp "${RESULT_FOLDER}"/sqlite_table_die_${tag}.csv  "${out_dir}/" 2>/dev/null || true
    cp "${RESULT_FOLDER}"/sqlite_table_${tag}.csv      "${out_dir}/" 2>/dev/null || true
    cp "${RESULT_FOLDER}"/sqlite_row_${tag}.csv        "${out_dir}/" 2>/dev/null || true
    if [[ -r "$SQLITE_DIE_AFFINITY_STATS_PATH" ]]; then
        {
            echo "[die_affinity_stats]"
            cat "$SQLITE_DIE_AFFINITY_STATS_PATH"
        } >>"$init_txt" 2>/dev/null || true
        cp "$SQLITE_DIE_AFFINITY_STATS_PATH" \
           "${RESULT_FOLDER%/}/sqlite_die_affinity_stats_${tag}.txt" 2>/dev/null || true
        cp "${RESULT_FOLDER%/}/sqlite_die_affinity_stats_${tag}.txt" "${out_dir}/" 2>/dev/null || true
    fi
    if [[ -r "$SQLITE_TEST_PHASE_STATS_PATH" ]]; then
        {
            echo "[test_phase_stats]"
            cat "$SQLITE_TEST_PHASE_STATS_PATH"
        } >>"$init_txt" 2>/dev/null || true
        cp "$SQLITE_TEST_PHASE_STATS_PATH" \
           "${RESULT_FOLDER%/}/sqlite_test_phase_stats_${tag}.txt" 2>/dev/null || true
        cp "${RESULT_FOLDER%/}/sqlite_test_phase_stats_${tag}.txt" "${out_dir}/" 2>/dev/null || true
    fi

    echo "=== Done: variant=$variant threads=$threads ==="
    echo "  Output: $init_txt"

    source resetdevice.sh
    sleep 1
}

# ---------- main ----------
./disablemeta.sh

mkdir -p "$DIE_RESULT_BASE"

for threads in $THREAD_COUNTS; do
    for variant in $VARIANTS; do
        run_one_test "$variant" "$threads"
    done
done

./enablemeta.sh

echo ""
echo "========================================"
echo "  [TEST1-RRPAD-FULLSCAN-OVERWRITE] All tests completed."
echo "  Results in: $DIE_RESULT_BASE"
echo "========================================"
