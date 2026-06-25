#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

source commonvariables.sh

VARIANTS="${VARIANTS:-die_base die_no1 die_no2 die_no3 die_all}"
ACCESS_DIST="${ACCESS_DIST:-normal}"
FILE_COUNT="${FILE_COUNT:-5000}"
INIT_SIZE_BYTES="${INIT_SIZE_BYTES:-131072}"
APPEND_SIZE_BYTES="${APPEND_SIZE_BYTES:-32768}"
TARGET_TOTAL_BYTES="${TARGET_TOTAL_BYTES:-6442450944}"
READ_OPS="${READ_OPS:-50000}"
COVER_ALL="${COVER_ALL:-1}"
APPEND_SYNC_EVERY="${APPEND_SYNC_EVERY:-1}"
NORMAL_MEAN="${NORMAL_MEAN:--1}"
NORMAL_STDDEV="${NORMAL_STDDEV:-400}"
ZIPF_SEED="${ZIPF_SEED:-42}"
NORMAL_SEED="${NORMAL_SEED:-314159}"
EXP_SEED="${EXP_SEED:-4242}"
ZIPF_ALPHA="${ZIPF_ALPHA:-1.2}"
EXP_LAMBDA="${EXP_LAMBDA:-0.0008}"
APPEND_ROUNDS="${APPEND_ROUNDS:-}"

NVMEV_DIR="${SCRIPT_DIR}/../nvmevirt_DA"
DRIVER="${SCRIPT_DIR}/filebench/fileserver_heat_access.py"
FILE_TIER_HELPER="${SCRIPT_DIR}/filebench/fileserver_file_tier.py"
RESULT_PREFIX="fileserver_heat"
DIE_RESULT_BASE="${RESULT_FOLDER%/}/${RESULT_PREFIX}_die_variants"
TEST_PHASE_PATH="${TEST_PHASE_PATH:-/sys/kernel/debug/nvmev/ftl0/test_phase}"
TEST_PHASE_STATS_PATH="${TEST_PHASE_STATS_PATH:-/sys/kernel/debug/nvmev/ftl0/test_phase_stats}"
DIE_STATS_RESET_PATH="${DIE_STATS_RESET_PATH:-/sys/module/nvmev/parameters/die_stats_reset}"
DIE_STATS_PATH="${DIE_STATS_PATH:-/sys/module/nvmev/parameters/die_stats}"
BG_NAND_STATS_PATH="${BG_NAND_STATS_PATH:-/sys/module/nvmev/parameters/bg_nand_stats}"
FTL_ROOT="${FTL_ROOT:-/sys/kernel/debug/nvmev/ftl0}"

if [[ -z "$APPEND_ROUNDS" ]]; then
    init_total_bytes=$((FILE_COUNT * INIT_SIZE_BYTES))
    if (( TARGET_TOTAL_BYTES <= init_total_bytes )); then
        APPEND_ROUNDS=0
    else
        APPEND_ROUNDS=$(((TARGET_TOTAL_BYTES - init_total_bytes) / APPEND_SIZE_BYTES))
    fi
fi

drop_caches() {
    sync
    echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
}

set_test_phase() {
    local value="$1"
    if [[ -w "$TEST_PHASE_PATH" ]]; then
        echo "$value" | sudo tee "$TEST_PHASE_PATH" >/dev/null
    fi
}

reset_die_stats() {
    if [[ -w "$DIE_STATS_RESET_PATH" ]]; then
        echo 1 | sudo tee "$DIE_STATS_RESET_PATH" >/dev/null
    fi
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

capture_ftl_views() {
    local state_tag="$1"
    local out_dir="$2"
    local prefix="${RESULT_FOLDER%/}/${RESULT_PREFIX}"

    if [[ -r "${FTL_ROOT}/die_affinity_stats" ]]; then
        cp "${FTL_ROOT}/die_affinity_stats" \
           "${prefix}_die_affinity_${state_tag}.txt" 2>/dev/null || true
    fi
    if [[ -r "${FTL_ROOT}/lpn_die_change_stats" ]]; then
        cp "${FTL_ROOT}/lpn_die_change_stats" \
           "${prefix}_lpn_die_change_${state_tag}.txt" 2>/dev/null || true
    fi
    if [[ -r "${FTL_ROOT}/page_tier" ]]; then
        cp "${FTL_ROOT}/page_tier" \
           "${prefix}_page_tier_${state_tag}.txt" 2>/dev/null || true
    fi
    if [[ -r "${FTL_ROOT}/access_count" ]]; then
        cp "${FTL_ROOT}/access_count" \
           "${prefix}_access_count_${state_tag}.txt" 2>/dev/null || true
    fi
    if [[ -r "${FTL_ROOT}/page_die" ]]; then
        cp "${FTL_ROOT}/page_die" \
           "${prefix}_page_die_${state_tag}.txt" 2>/dev/null || true
    fi
    if [[ -r "$TEST_PHASE_STATS_PATH" ]]; then
        cp "$TEST_PHASE_STATS_PATH" \
           "${prefix}_test_phase_stats_${state_tag}.txt" 2>/dev/null || true
    fi
    if [[ -r "$DIE_STATS_PATH" ]]; then
        cp "$DIE_STATS_PATH" \
           "${prefix}_die_stats_${state_tag}.txt" 2>/dev/null || true
    fi
    if [[ -r "$BG_NAND_STATS_PATH" ]]; then
        cp "$BG_NAND_STATS_PATH" \
           "${prefix}_bg_nand_stats_${state_tag}.txt" 2>/dev/null || true
    fi

    cp "${prefix}"_*_"${state_tag}".txt "$out_dir/" 2>/dev/null || true
}

run_file_tier_analysis() {
    local variant="$1"
    local out_dir="$2"
    local root="$3"
    local plan_csv="$4"
    local prefix="${RESULT_FOLDER%/}/${RESULT_PREFIX}"
    local page_tier_path="${prefix}_page_tier_${variant}.txt"
    local page_die_path="${prefix}_page_die_${variant}.txt"
    local access_count_path="${prefix}_access_count_${variant}.txt"
    local out_csv="${prefix}_file_tier_${variant}.csv"
    local out_summary="${prefix}_file_tier_${variant}.txt"
    local lpn_manifest="${prefix}_file_lpns_${variant}.csv"

    if [[ ! -r "$page_tier_path" || ! -r "$page_die_path" || ! -r "$access_count_path" ]]; then
        return 0
    fi

    python3 "$FILE_TIER_HELPER" \
        --root "$root" \
        --page-tier "$page_tier_path" \
        --page-die "$page_die_path" \
        --access-count "$access_count_path" \
        --out-csv "$out_csv" \
        --out-summary "$out_summary" \
        --plan-csv "$plan_csv" \
        --lpn-manifest-in "$lpn_manifest" \
        --lpn-manifest-out "$lpn_manifest"

    cp "$out_csv" "$out_dir/" 2>/dev/null || true
    cp "$out_summary" "$out_dir/" 2>/dev/null || true
    cp "$lpn_manifest" "$out_dir/" 2>/dev/null || true
}

distribution_seed() {
    case "$ACCESS_DIST" in
        zipf) printf '%s' "$ZIPF_SEED" ;;
        normal) printf '%s' "$NORMAL_SEED" ;;
        exp) printf '%s' "$EXP_SEED" ;;
        *)
            echo "ERROR: ACCESS_DIST must be one of normal|zipf|exp" >&2
            exit 1
            ;;
    esac
}

run_one_variant() {
    local variant="$1"
    local out_dir="${DIE_RESULT_BASE}/${variant}"
    local init_txt="${RESULT_FOLDER%/}/${RESULT_PREFIX}_init_${variant}.txt"
    local append_log="${RESULT_FOLDER%/}/${RESULT_PREFIX}_append_${variant}.txt"
    local read_log="${RESULT_FOLDER%/}/${RESULT_PREFIX}_read_${variant}.txt"
    local state_csv="${RESULT_FOLDER%/}/${RESULT_PREFIX}_state_${variant}.csv"
    local append_plan_csv="${RESULT_FOLDER%/}/${RESULT_PREFIX}_append_plan_${variant}.csv"
    local read_plan_csv="${RESULT_FOLDER%/}/${RESULT_PREFIX}_read_plan_${variant}.csv"
    local root="${TARGET_FOLDER%/}/bigfileset"
    local seed

    seed="$(distribution_seed)"
    mkdir -p "$out_dir"
    : >"$init_txt"

    {
        echo "[fileserver_heat_init] variant=${variant}"
        echo "[fileserver_heat_init] dist=${ACCESS_DIST} seed=${seed}"
        echo "[fileserver_heat_init] files=${FILE_COUNT} init_size_bytes=${INIT_SIZE_BYTES}"
        echo "[fileserver_heat_init] append_size_bytes=${APPEND_SIZE_BYTES} append_rounds=${APPEND_ROUNDS}"
        echo "[fileserver_heat_init] target_total_bytes=${TARGET_TOTAL_BYTES}"
        echo "[fileserver_heat_init] read_ops=${READ_OPS} cover_all=${COVER_ALL}"
        echo "[fileserver_heat_init] append_sync_every=${APPEND_SYNC_EVERY}"
    } >>"$init_txt"

    drop_caches
    sleep 2

    load_die_module "$variant"
    lsblk
    source setdevice.sh
    sleep 1

    echo 0 | sudo tee /sys/block/${DATA_NAME}/queue/read_ahead_kb >/dev/null 2>&1 || true
    echo "[readahead] set /sys/block/${DATA_NAME}/queue/read_ahead_kb = $(cat /sys/block/${DATA_NAME}/queue/read_ahead_kb 2>/dev/null || echo N/A)" \
        | tee -a "$init_txt"

    numactl --cpubind="$NUMADOMAIN" --membind="$NUMADOMAIN" \
        python3 "$DRIVER" \
            --phase create-append \
            --root "$root" \
            --dist "$ACCESS_DIST" \
            --seed "$seed" \
            --normal-mean "$NORMAL_MEAN" \
            --normal-stddev "$NORMAL_STDDEV" \
            --zipf-alpha "$ZIPF_ALPHA" \
            --exp-lambda "$EXP_LAMBDA" \
            --file-count "$FILE_COUNT" \
            --init-size-bytes "$INIT_SIZE_BYTES" \
            --append-size-bytes "$APPEND_SIZE_BYTES" \
            --append-rounds "$APPEND_ROUNDS" \
            --sync-every "$APPEND_SYNC_EVERY" \
            --state-out "$state_csv" \
            --plan-out "$append_plan_csv" \
            --label "append-${variant}" | tee "$append_log"

    capture_ftl_views "${variant}_append" "$out_dir"
    drop_caches
    sleep 2

    set_test_phase 1
    reset_die_stats

    numactl --cpubind="$NUMADOMAIN" --membind="$NUMADOMAIN" \
        python3 "$DRIVER" \
            --phase read \
            --root "$root" \
            --dist "$ACCESS_DIST" \
            --seed "$seed" \
            --read-ops "$READ_OPS" \
            --cover-all "$COVER_ALL" \
            --state-in "$state_csv" \
            --plan-out "$read_plan_csv" \
            --label "read-${variant}" | tee "$read_log"

    set_test_phase 0
    capture_ftl_views "$variant" "$out_dir"
    run_file_tier_analysis "$variant" "$out_dir" "$root" "$read_plan_csv"

    {
        echo
        echo "[append_log]"
        cat "$append_log"
        echo
        echo "[read_log]"
        cat "$read_log"
        if [[ -r "${RESULT_FOLDER%/}/${RESULT_PREFIX}_test_phase_stats_${variant}.txt" ]]; then
            echo
            echo "[test_phase_stats]"
            cat "${RESULT_FOLDER%/}/${RESULT_PREFIX}_test_phase_stats_${variant}.txt"
        fi
        if [[ -r "${RESULT_FOLDER%/}/${RESULT_PREFIX}_die_stats_${variant}.txt" ]]; then
            echo
            echo "[die_stats]"
            cat "${RESULT_FOLDER%/}/${RESULT_PREFIX}_die_stats_${variant}.txt"
        fi
        if [[ -r "${RESULT_FOLDER%/}/${RESULT_PREFIX}_bg_nand_stats_${variant}.txt" ]]; then
            echo
            echo "[bg_nand_stats]"
            cat "${RESULT_FOLDER%/}/${RESULT_PREFIX}_bg_nand_stats_${variant}.txt"
        fi
    } >>"$init_txt"

    cp "$init_txt" "$out_dir/" 2>/dev/null || true
    cp "$append_log" "$out_dir/" 2>/dev/null || true
    cp "$read_log" "$out_dir/" 2>/dev/null || true
    cp "$state_csv" "$out_dir/" 2>/dev/null || true
    cp "$append_plan_csv" "$out_dir/" 2>/dev/null || true
    cp "$read_plan_csv" "$out_dir/" 2>/dev/null || true

    source resetdevice.sh
    sleep 1
}

echo 0 > /proc/sys/kernel/randomize_va_space
./disablemeta.sh
mkdir -p "$DIE_RESULT_BASE"

for variant in $VARIANTS; do
    run_one_variant "$variant"
done

./enablemeta.sh

echo ""
echo "========================================"
echo "  [FILESERVER-HEAT] All tests completed."
echo "  Results in: $DIE_RESULT_BASE"
echo "========================================"
