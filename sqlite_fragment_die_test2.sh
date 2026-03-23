#!/bin/bash -e
#
# sqlite_fragment_die_test2.sh
#
# Die-affinity test 2: WORST-CASE die collision via dummy stripe padding.
#   Row size = 32KB (= 1 die allocation granularity)
#   After each row write, a small dummy pad is written to maintain worst-case die collision.
#   This forces every table's rows onto the SAME die → maximum collision (8× penalty).
#   Default data = 6GB with ~2GB dummy padding.
#
# Compares 4 FTL variants:
#   die_base: QLC hot/cold OFF + die affinity OFF
#   die_no1:  QLC hot/cold OFF + die affinity ON
#   die_no2:  QLC hot/cold ON  + die affinity OFF
#   die_all:  QLC hot/cold ON  + die affinity ON
#
# Prerequisites:
#   cd nvmevirt_DA && ./build_die.sh all

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

source commonvariables.sh

# ---------- tunables ----------
THREAD_COUNTS="${THREAD_COUNTS:-1 4 8}"
COLD_READS_PER_TBL="${COLD_READS_PER_TBL:-5000}"
VARIANTS="${VARIANTS:-die_base die_no1 die_no2 die_no3 die_all}"

SQLITE_TARGET_BYTES=${SQLITE_TARGET_BYTES:-6G}
SQLITE_TABLE_COUNT=${SQLITE_TABLE_COUNT:-128}
SQLITE_ROWS_PER_TABLE=${SQLITE_ROWS_PER_TABLE:-1536}
SQLITE_INTERLEAVE_ROWS=${SQLITE_INTERLEAVE_ROWS:-6144}
SQLITE_INTERLEAVE_READS=${SQLITE_INTERLEAVE_READS:-1600}
SQLITE_PAGE_TIER_PATH=${SQLITE_PAGE_TIER_PATH:-/sys/kernel/debug/nvmev/ftl0/page_tier}
SQLITE_ACCESS_COUNT_PATH=${SQLITE_ACCESS_COUNT_PATH:-/sys/kernel/debug/nvmev/ftl0/access_count}
SQLITE_FTL_HOST_PAGE_BYTES=${SQLITE_FTL_HOST_PAGE_BYTES:-4K}
SQLITE_DIRECT_IO=${SQLITE_DIRECT_IO:-1}
SQLITE_FAST_INIT_PROFILE=${SQLITE_FAST_INIT_PROFILE:-1}
SQLITE_DUMMY_STRIPE_BYTES=${SQLITE_DUMMY_STRIPE_BYTES:-10922}
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
DIE_RESULT_BASE="${RESULT_FOLDER%/}/die_test2_worstcase"

EXE_NAME="sqlite_append_die_affinity"

if [[ ! -f ./${EXE_NAME} ]] || [[ $FORCE_REBUILD == 1 ]]; then
    echo "=== Compiling ${EXE_NAME} (32KB rows, die affinity test) ==="
    gcc -D_GNU_SOURCE \
        -DTARGET_FOLDER="\"$TARGET_FOLDER\"" \
        -DRESULT_FOLDER="\"$RESULT_FOLDER\"" \
        -o ./${EXE_NAME} \
        ./$SRC_PATH/sqlite_append_die_affinity.c \
        -lsqlite3 -lm -lpthread
fi

mkdir -p "$RESULT_FOLDER"
mkdir -p "$TARGET_FOLDER"

drop_caches() {
    sync
    echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
}

post_run_settle() {
    local settle_sec="${SQLITE_POST_RUN_SETTLE_SEC:-3}"

    sync
    sleep "$settle_sec"
}

partitions_still_present() {
    lsblk -nr -o NAME "$DATA_DEV" 2>/dev/null | grep -qx "$DATA_PAT_NAME" && return 0
    lsblk -nr -o NAME "$JOURNAL_DEV" 2>/dev/null | grep -qx "$JOURNAL_PAT_NAME" && return 0
    return 1
}

cleanup_after_test() {
    local max_retry="${SQLITE_RESET_RETRIES:-3}"
    local attempt=1

    while (( attempt <= max_retry )); do
        source resetdevice.sh
        if ! partitions_still_present; then
            return 0
        fi

        echo "[reset] partitions still visible after cleanup (attempt ${attempt}/${max_retry}); waiting before retry"
        sync
        sleep 3
        attempt=$((attempt + 1))
    done

    echo "ERROR: cleanup did not remove stale partitions after ${max_retry} attempts" >&2
    return 1
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
    local tag="die_worst_${variant}_t${threads}"
    local init_txt="${RESULT_FOLDER%/}/sqlite_die_worst_init_${variant}_t${threads}.txt"
    local out_dir="${DIE_RESULT_BASE}/${variant}/t${threads}"

    mkdir -p "$out_dir"

    echo ""
    echo "================================================================"
    echo "  [TEST2-WORSTCASE] variant=$variant  threads=$threads  tag=$tag"
    echo "  row_bytes=32KB  tables=$SQLITE_TABLE_COUNT  rows/tbl=$SQLITE_ROWS_PER_TABLE"
    echo "  target=$SQLITE_TARGET_BYTES  dummy=ON  dummy_stripe=$SQLITE_DUMMY_STRIPE_BYTES"
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
        --cold-full-read-mode random-concurrent \
        --cold-concurrent-threads "$threads" \
        --cold-random-reads-per-tbl "$COLD_READS_PER_TBL" \
        --strict-cold-per-select \
        --dummy-interleave \
        --dummy-stripe-bytes "$SQLITE_DUMMY_STRIPE_BYTES" \
        "${extra_args[@]}" \
        --tag "$tag" \
        >"$init_txt" 2>&1

    cp "$init_txt" "${out_dir}/" 2>/dev/null || true
    cp "${RESULT_FOLDER}"/sqlite_table_tier_${tag}.csv "${out_dir}/" 2>/dev/null || true
    cp "${RESULT_FOLDER}"/sqlite_table_${tag}.csv      "${out_dir}/" 2>/dev/null || true
    cp "${RESULT_FOLDER}"/sqlite_row_${tag}.csv         "${out_dir}/" 2>/dev/null || true

    echo "=== Done: variant=$variant threads=$threads ==="
    echo "  Output: $init_txt"

    post_run_settle
    cleanup_after_test
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
echo "  [TEST2] All worst-case collision tests completed."
echo "  Results in: $DIE_RESULT_BASE"
echo "========================================"
