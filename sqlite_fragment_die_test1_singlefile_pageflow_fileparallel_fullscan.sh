#!/bin/bash
set -e
#
# sqlite_fragment_die_test1_singlefile_pageflow_fileparallel_fullscan.sh
#
# Die-affinity test 1 (SINGLEFILE PAGEFLOW + FILE-PARALLEL FULL SCAN):
#   - one SQLite DB file containing many logical tables
#   - init appends tables in pageflow order inside that single DB
#   - cold read preserves the tablefile fileparallel harness controls
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

source commonvariables.sh

# ---------- tunables ----------
THREAD_COUNTS="${THREAD_COUNTS:-1 8}"
VARIANTS="${VARIANTS:-die_base die_no1 die_base_lru die_no4}"

SQLITE_TARGET_BYTES=${SQLITE_TARGET_BYTES:-10G}
SQLITE_TABLE_COUNT=${SQLITE_TABLE_COUNT:-80}
SQLITE_ROWS_PER_TABLE=${SQLITE_ROWS_PER_TABLE:-0}
SQLITE_WINDOW_TABLES=${SQLITE_WINDOW_TABLES:-80}
# Window sweep for each per-table append burst. 960 pages is ~3.75MB,
# 2048 pages is exactly 8MB with 4KB host pages, and 2304 pages is ~9MB.
SQLITE_WINDOW_PAGES_PER_TABLE_LIST=${SQLITE_WINDOW_PAGES_PER_TABLE_LIST:-${SQLITE_WINDOW_PAGES_PER_TABLE:-"960 2048 2304"}}
SQLITE_WINDOW_PAGES_PER_TABLE=${SQLITE_WINDOW_PAGES_PER_TABLE:-2304}
SQLITE_WINDOW_PASSES_PER_ROUND=${SQLITE_WINDOW_PASSES_PER_ROUND:-1}
# Default matches the workload binary's coarse init-read interval (~10 events
# for an 8GiB/4KiB run).  Set SQLITE_INTERLEAVE_PAGES=window only when an
# intentionally dense preheat event after every window_pages is needed.
SQLITE_INTERLEAVE_PAGES=${SQLITE_INTERLEAVE_PAGES:-209715}
SQLITE_INTERLEAVE_READS=${SQLITE_INTERLEAVE_READS:-1000}
SQLITE_INIT_DROP_CACHE_EACH_READ=${SQLITE_INIT_DROP_CACHE_EACH_READ:-1}
SQLITE_ANALYZE_LATENCY_RUN=${SQLITE_ANALYZE_LATENCY_RUN:-1}
SQLITE_CAPTURE_DMESG_TAIL=${SQLITE_CAPTURE_DMESG_TAIL:-1}
SQLITE_DMESG_TAIL_LINES=${SQLITE_DMESG_TAIL_LINES:-2000}
SQLITE_DMESG_TIME_SLACK_S=${SQLITE_DMESG_TIME_SLACK_S:-1}
SQLITE_DMESG_GREP=${SQLITE_DMESG_GREP:-"NVMeVirt|nvmev|blocked for more than|hung_task|No free entry|SLOW_PATH|credit scan capped|credit horizon overflow|proc queue full|writeback proc queue full|write buffer allocation|blk_update_request|Buffer I/O error|critical medium error"}
SQLITE_REFSTYLE_DUMMY_BYTES=${SQLITE_REFSTYLE_DUMMY_BYTES:-0}
SQLITE_ALIGN_PAGES=${SQLITE_ALIGN_PAGES:-0}
SQLITE_DIE_AFFINITY_STATS_PATH=${SQLITE_DIE_AFFINITY_STATS_PATH:-/sys/kernel/debug/nvmev/ftl0/die_affinity_stats}
SQLITE_LPN_DIE_CHANGE_STATS_PATH=${SQLITE_LPN_DIE_CHANGE_STATS_PATH:-/sys/kernel/debug/nvmev/ftl0/lpn_die_change_stats}
SQLITE_TEST_PHASE_PATH=${SQLITE_TEST_PHASE_PATH:-/sys/kernel/debug/nvmev/ftl0/test_phase}
SQLITE_HEAT_EPOCH_PATH=${SQLITE_HEAT_EPOCH_PATH:-/sys/kernel/debug/nvmev/ftl0/heat_epoch}
SQLITE_TEST_PHASE_STATS_PATH=${SQLITE_TEST_PHASE_STATS_PATH:-/sys/kernel/debug/nvmev/ftl0/test_phase_stats}
SQLITE_SUPERBLOCK_STATS_PATH=${SQLITE_SUPERBLOCK_STATS_PATH:-/sys/kernel/debug/nvmev/ftl0/superblock_stats}
SQLITE_DIE_STATS_PATH=${SQLITE_DIE_STATS_PATH:-/sys/module/nvmev/parameters/die_stats}
SQLITE_BG_NAND_STATS_PATH=${SQLITE_BG_NAND_STATS_PATH:-/sys/module/nvmev/parameters/bg_nand_stats}
SQLITE_GC_NAND_TIMING=${SQLITE_GC_NAND_TIMING:-1}
SQLITE_GC_NAND_TIMING_PATH=${SQLITE_GC_NAND_TIMING_PATH:-/sys/module/nvmev/parameters/gc_nand_timing}
SQLITE_FTL_HOST_PAGE_BYTES=${SQLITE_FTL_HOST_PAGE_BYTES:-4K}
SQLITE_DIRECT_IO=${SQLITE_DIRECT_IO:-1}
SQLITE_FAST_INIT_PROFILE=${SQLITE_FAST_INIT_PROFILE:-0}
SQLITE_COLD_FULL_READ_ITERS=${SQLITE_COLD_FULL_READ_ITERS:-1}
SQLITE_MAP_CMT_BYTES=${SQLITE_MAP_CMT_BYTES:-8M}
SQLITE_MAP_CMT_BYTES_LIST=${SQLITE_MAP_CMT_BYTES_LIST:-$SQLITE_MAP_CMT_BYTES}
SQLITE_REBUILD_DIE_MODULES=${SQLITE_REBUILD_DIE_MODULES:-0}
# Multiple modes are allowed, e.g. "quota-page-shuffled quota-row-shuffled".
# Recommended mapping-cache isolation mode: quota-page-shuffled.
SQLITE_COLD_FULL_READ_MODE=${SQLITE_COLD_FULL_READ_MODE:-full-scan-concurrent}
SQLITE_COLD_EXTRA_APPEND_BYTES=${SQLITE_COLD_EXTRA_APPEND_BYTES:-0}
SQLITE_COLD_EXTRA_MODE=${SQLITE_COLD_EXTRA_MODE:-off}
SQLITE_COLD_EXTRA_READ_RATIO=${SQLITE_COLD_EXTRA_READ_RATIO:-10}
SQLITE_COLD_EXTRA_ROW_READS_PER_BATCH=${SQLITE_COLD_EXTRA_ROW_READS_PER_BATCH:-0}
SQLITE_COLD_EXTRA_SLEEP_US=${SQLITE_COLD_EXTRA_SLEEP_US:-0}
SQLITE_COLD_EXTRA_READ_SLEEP_EVERY=${SQLITE_COLD_EXTRA_READ_SLEEP_EVERY:-0}
SQLITE_COLD_EXTRA_WRITE_SLEEP_EVERY=${SQLITE_COLD_EXTRA_WRITE_SLEEP_EVERY:-0}
SQLITE_TEST_PHASE_RECENT_WRITE_GUARD=${SQLITE_TEST_PHASE_RECENT_WRITE_GUARD:-1}
SQLITE_TEST_PHASE_GUARD_READ_REQS=${SQLITE_TEST_PHASE_GUARD_READ_REQS:-256}
SQLITE_TEST_PHASE_RECENT_WRITE_GUARD_PATH=${SQLITE_TEST_PHASE_RECENT_WRITE_GUARD_PATH:-/sys/module/nvmev/parameters/test_phase_recent_write_guard}
SQLITE_TEST_PHASE_GUARD_READ_REQS_PATH=${SQLITE_TEST_PHASE_GUARD_READ_REQS_PATH:-/sys/module/nvmev/parameters/test_phase_guard_read_reqs}
SQLITE_ACCESS_DIST=${SQLITE_ACCESS_DIST:-zipf}
SQLITE_ACCESS_DIST_LIST=${SQLITE_ACCESS_DIST_LIST:-$SQLITE_ACCESS_DIST}
NORMAL_MEAN=${NORMAL_MEAN:--1}
NORMAL_STDDEV=${NORMAL_STDDEV:-400}
NORMAL_SEED=${NORMAL_SEED:-314159}
ZIPF_SEED=${ZIPF_SEED:-42}
EXP_SEED=${EXP_SEED:-4242}
ZIPF_ALPHA=${ZIPF_ALPHA:-0.75}
EXP_LAMBDA=${EXP_LAMBDA:-0.0008}
# ---------- end tunables ----------

SRC_PATH="sqlite"
NVMEV_DIR="${SCRIPT_DIR}/../nvmevirt_DA"
DIE_RESULT_BASE="${RESULT_FOLDER%/}/die_test1_singlefile_pageflow_fileparallel_fullscan"

EXE_NAME="sqlite_append_die_affinity_singlefile_pageflow_fileparallel"
SRC_FILE="./$SRC_PATH/sqlite_append_die_affinity_singlefile_pageflow_fileparallel.c"

if [[ ! -f ./${EXE_NAME} ]] || [[ $FORCE_REBUILD == 1 ]]; then
    echo "=== Compiling ${EXE_NAME} (singlefile pageflow file-parallel full-scan test) ==="
    gcc -D_GNU_SOURCE \
        -DTARGET_FOLDER="\"$TARGET_FOLDER\"" \
        -DRESULT_FOLDER="\"$RESULT_FOLDER\"" \
        -o ./${EXE_NAME} \
        "${SRC_FILE}" \
        -lsqlite3 -lm -lpthread
fi

mkdir -p "$RESULT_FOLDER"
mkdir -p "$TARGET_FOLDER"

if [[ "$SQLITE_REBUILD_DIE_MODULES" == 1 ]]; then
    echo "=== Rebuilding die modules: $VARIANTS ==="
    (cd "$NVMEV_DIR" && bash build_die.sh $VARIANTS)
fi

print_init_log_tail() {
    local path="$1"
    if [[ -f "$path" ]]; then
        echo "----- tail: $path -----" >&2
        tail -n 40 "$path" >&2 || true
        echo "------------------------" >&2
    fi
}

dmesg_line_count() {
    if [[ "$SQLITE_CAPTURE_DMESG_TAIL" != "1" ]]; then
        echo 0
        return 0
    fi
    if ! command -v dmesg >/dev/null 2>&1; then
        echo 0
        return 0
    fi
    dmesg 2>/dev/null | wc -l | tr -d ' ' || echo 0
}

dmesg_boot_seconds() {
    if [[ -r /proc/uptime ]]; then
        awk '{print $1}' /proc/uptime 2>/dev/null || true
    fi
}

dmesg_has_raw_timestamps() {
    dmesg "$@" 2>/dev/null \
        | head -n 50 \
        | grep -qE '^\[[[:space:]]*[0-9]+(\.[0-9]+)?\]'
}

capture_kernel_tail() {
    local tag="$1"
    local init_txt="$2"
    local out_dir="$3"
    local start_line="${4:-0}"
    local start_boot_s="${5:-}"
    local dmesg_file="${RESULT_FOLDER%/}/sqlite_dmesg_tail_${tag}.log"
    local end_line=0
    local mode="line"
    local start_filter_s="${start_boot_s:-0}"

    if [[ "$SQLITE_CAPTURE_DMESG_TAIL" != "1" ]]; then
        echo "[kernel_tail_status] status=disabled mode=none start_line=$start_line end_line=0 start_boot_s=${start_boot_s:-0} start_filter_s=${start_filter_s:-0} time_slack_s=$SQLITE_DMESG_TIME_SLACK_S matches=0" >>"$init_txt" 2>/dev/null || true
        return 0
    fi
    if ! command -v dmesg >/dev/null 2>&1; then
        echo "[kernel_tail_status] status=unavailable mode=none start_line=$start_line end_line=0 start_boot_s=${start_boot_s:-0} start_filter_s=${start_filter_s:-0} time_slack_s=$SQLITE_DMESG_TIME_SLACK_S matches=0" >>"$init_txt" 2>/dev/null || true
        return 0
    fi
    end_line="$(dmesg_line_count)"
    if [[ "$end_line" == "0" ]]; then
        echo "[kernel_tail_status] status=unavailable mode=none start_line=$start_line end_line=0 start_boot_s=${start_boot_s:-0} start_filter_s=${start_filter_s:-0} time_slack_s=$SQLITE_DMESG_TIME_SLACK_S matches=0" >>"$init_txt" 2>/dev/null || true
        return 0
    fi

    if [[ -n "$start_boot_s" ]] && dmesg_has_raw_timestamps; then
        mode="time-default"
        start_filter_s="$(awk -v start="$start_boot_s" -v slack="$SQLITE_DMESG_TIME_SLACK_S" 'BEGIN { v = start - slack; if (v < 0) v = 0; printf "%.6f", v }')"
        dmesg 2>/dev/null \
            | awk -v start="$start_filter_s" '
                match($0, /^\[[[:space:]]*[0-9]+(\.[0-9]+)?\]/) {
                    ts = substr($0, RSTART, RLENGTH)
                    sub(/^\[[[:space:]]*/, "", ts)
                    sub(/\]$/, "", ts)
                    if ((ts + 0) >= (start + 0)) print
                }
            ' \
            | grep -E "$SQLITE_DMESG_GREP" \
            | tail -n "$SQLITE_DMESG_TAIL_LINES" \
            >"$dmesg_file" 2>/dev/null || true
    elif [[ -n "$start_boot_s" ]] && dmesg_has_raw_timestamps --time-format=raw; then
        mode="time-raw"
        start_filter_s="$(awk -v start="$start_boot_s" -v slack="$SQLITE_DMESG_TIME_SLACK_S" 'BEGIN { v = start - slack; if (v < 0) v = 0; printf "%.6f", v }')"
        dmesg --time-format=raw 2>/dev/null \
            | awk -v start="$start_filter_s" '
                match($0, /^\[[[:space:]]*[0-9]+(\.[0-9]+)?\]/) {
                    ts = substr($0, RSTART, RLENGTH)
                    sub(/^\[[[:space:]]*/, "", ts)
                    sub(/\]$/, "", ts)
                    if ((ts + 0) >= (start + 0)) print
                }
            ' \
            | grep -E "$SQLITE_DMESG_GREP" \
            | tail -n "$SQLITE_DMESG_TAIL_LINES" \
            >"$dmesg_file" 2>/dev/null || true
    else
        mode="line"
        if [[ "$start_line" =~ ^[0-9]+$ && "$start_line" -gt 0 && "$end_line" -lt "$start_line" ]]; then
            echo "[kernel_tail_status] status=rotated mode=$mode start_line=$start_line end_line=$end_line start_boot_s=${start_boot_s:-0} start_filter_s=${start_filter_s:-0} time_slack_s=$SQLITE_DMESG_TIME_SLACK_S matches=0" >>"$init_txt" 2>/dev/null || true
            return 0
        fi
        if [[ "$start_line" =~ ^[0-9]+$ && "$start_line" -gt 0 ]]; then
            dmesg 2>/dev/null \
                | tail -n +"$((start_line + 1))" \
                | grep -E "$SQLITE_DMESG_GREP" \
                | tail -n "$SQLITE_DMESG_TAIL_LINES" \
                >"$dmesg_file" 2>/dev/null || true
        else
            dmesg 2>/dev/null \
                | grep -E "$SQLITE_DMESG_GREP" \
                | tail -n "$SQLITE_DMESG_TAIL_LINES" \
                >"$dmesg_file" 2>/dev/null || true
        fi
    fi

    echo "[kernel_tail_status] status=captured mode=$mode start_line=$start_line end_line=$end_line start_boot_s=${start_boot_s:-0} start_filter_s=${start_filter_s:-0} time_slack_s=$SQLITE_DMESG_TIME_SLACK_S matches=$(wc -l <"$dmesg_file" 2>/dev/null | tr -d ' ' || echo 0)" >>"$init_txt" 2>/dev/null || true
    if [[ -s "$dmesg_file" ]]; then
        {
            echo "[kernel_tail]"
            cat "$dmesg_file"
        } >>"$init_txt" 2>/dev/null || true
        cp "$dmesg_file" "$out_dir/" 2>/dev/null || true
    fi
}

validate_workload_outputs() {
    local tag="$1"
    local init_txt="$2"
    local expected_cold_mode="$3"
    local expected_log_cold_mode="$3"
    local ok=0
    local cold_extra_mixed=0

    case "$SQLITE_COLD_EXTRA_MODE" in
        serial-mixed|mixed|interleaved|write-read|write-then-read)
            cold_extra_mixed=1
            ;;
    esac

    if [[ "$SQLITE_COLD_EXTRA_MODE" == "concurrent" ]] &&
       [[ "$expected_cold_mode" == "random-row-concurrent" ]]; then
        expected_log_cold_mode="read-concurrent+append-concurrent"
    fi

    if [[ -f "${RESULT_FOLDER}/sqlite_table_tier_${tag}.csv" ]]; then
        ok=1
    fi
    if [[ -f "${RESULT_FOLDER}/sqlite_page_tier_${tag}.csv" ]]; then
        ok=1
    fi
    if [[ -f "${RESULT_FOLDER}/sqlite_table_die_${tag}.csv" ]]; then
        ok=1
    fi
    if [[ -f "${RESULT_FOLDER}/sqlite_table_${tag}.csv" ]]; then
        ok=1
    fi
    if [[ -f "${RESULT_FOLDER}/sqlite_row_${tag}.csv" ]]; then
        ok=1
    fi
    if grep -q "\\[sqlite_init\\] tag=${tag}" "$init_txt" 2>/dev/null; then
        ok=1
    fi

    if [[ "$ok" != "1" ]]; then
        return 1
    fi
    if [[ -n "$expected_log_cold_mode" ]]; then
        local cold_mode_ok=0
        if grep -q "\\[sqlite_init\\].*tag=${tag}.*cold_mode=${expected_log_cold_mode}" "$init_txt" 2>/dev/null; then
            cold_mode_ok=1
        elif [[ "$SQLITE_COLD_EXTRA_MODE" == "concurrent" ]] &&
             [[ "$expected_cold_mode" == "random-row-concurrent" ]] &&
             grep -q "\\[sqlite_init\\].*tag=${tag}.*cold_mode=${expected_cold_mode}" "$init_txt" 2>/dev/null; then
            cold_mode_ok=1
        fi
        if [[ "$cold_mode_ok" != "1" ]]; then
            echo "ERROR: cold mode mismatch for tag=${tag}; expected cold_mode=${expected_log_cold_mode}" >&2
            print_init_log_tail "$init_txt"
            return 1
        fi
    fi
    if [[ "$expected_cold_mode" == "quota-row-shuffled" ]] &&
       [[ "$cold_extra_mixed" != "1" ]] &&
       ! grep -q "\\[sqlite_cold_global\\]" "$init_txt" 2>/dev/null; then
        echo "ERROR: quota-row-shuffled run did not emit sqlite_cold_global logs for tag=${tag}" >&2
        print_init_log_tail "$init_txt"
        return 1
    fi
    if [[ "$expected_cold_mode" == "quota-page-shuffled" ]] &&
       ! grep -q "\\[sqlite_cold_global_page\\]" "$init_txt" 2>/dev/null; then
        echo "ERROR: quota-page-shuffled run did not emit sqlite_cold_global_page logs for tag=${tag}" >&2
        print_init_log_tail "$init_txt"
        return 1
    fi

    return 0
}

drop_caches() {
    sync
    echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
}

size_to_bytes() {
    local raw="$1"
    local num unit

    if [[ "$raw" =~ ^([0-9]+)([KkMmGg]?)$ ]]; then
        num="${BASH_REMATCH[1]}"
        unit="${BASH_REMATCH[2]}"
    else
        echo "ERROR: invalid size '$raw'; use integer bytes or K/M/G suffix" >&2
        return 1
    fi

    case "$unit" in
        K|k) echo $((num * 1024)) ;;
        M|m) echo $((num * 1024 * 1024)) ;;
        G|g) echo $((num * 1024 * 1024 * 1024)) ;;
        *) echo "$num" ;;
    esac
}

size_tag() {
    printf '%s' "$1" | tr -c 'A-Za-z0-9' '_'
}

load_die_module() {
    local variant="$1"
    local map_cmt_bytes="$2"
    local extra_module_params=""
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
    case "$variant" in
        die_base_lru|die_no4)
            extra_module_params="map_cmt_bytes=${map_cmt_bytes}"
            ;;
    esac
    NVMEV_EXTRA_MODULE_PARAMS="$extra_module_params" ./nvmevstart_on.sh
    if [[ -f ./nvmev_on.ko.die_bak ]]; then
        mv ./nvmev_on.ko.die_bak ./nvmev_on.ko
    fi
    sleep 1
}

set_optional_module_param() {
    local path="$1"
    local value="$2"
    local label="$3"

    if [[ -z "$path" || -z "$value" ]]; then
        return 0
    fi
    if [[ ! -e "$path" ]]; then
        echo "[module_param] skip $label: $path not present"
        return 0
    fi
    if echo "$value" | sudo tee "$path" >/dev/null; then
        echo "[module_param] $label=$value path=$path"
    else
        echo "[module_param] failed to set $label=$value path=$path" >&2
    fi
}

configure_test_phase_guard_params() {
    set_optional_module_param "$SQLITE_TEST_PHASE_RECENT_WRITE_GUARD_PATH" \
        "$SQLITE_TEST_PHASE_RECENT_WRITE_GUARD" "test_phase_recent_write_guard"
    set_optional_module_param "$SQLITE_TEST_PHASE_GUARD_READ_REQS_PATH" \
        "$SQLITE_TEST_PHASE_GUARD_READ_REQS" "test_phase_guard_read_reqs"
}

debugfs_sibling_files() {
    local path="$1"
    local leaf
    local dir
    local parent
    local base

    [[ -n "$path" ]] || return 1
    leaf="$(basename "$path")"
    dir="$(dirname "$path")"
    base="$(basename "$dir")"
    if [[ "$base" =~ ^ftl[0-9]+$ ]]; then
        parent="$(dirname "$dir")"
        for f in "$parent"/ftl*/"$leaf"; do
            [[ -e "$f" ]] && printf '%s\n' "$f"
        done | sort -V
    elif [[ -r "$path" ]]; then
        printf '%s\n' "$path"
    fi
}

write_numeric_stats_aggregate() {
    local path="$1"
    local out_file="$2"
    local files=()

    mapfile -t files < <(debugfs_sibling_files "$path" | while read -r f; do
        [[ -r "$f" ]] && printf '%s\n' "$f"
    done)
    [[ ${#files[@]} -gt 0 ]] || return 1

    awk '
        NF == 2 && $2 ~ /^[0-9]+$/ {
            if (!seen[$1]++) order[++n] = $1
            sum[$1] += $2
        }
        END {
            for (i = 1; i <= n; i++) print order[i], sum[order[i]]
        }
    ' "${files[@]}" >"$out_file"
}

run_one_test() {
    local variant="$1"
    local threads="$2"
    local cold_mode="$3"
    local cmt_label="$4"
    local cmt_bytes="$5"
    local window_pages="$6"
    local mode_tag
    local cmt_tag
    local dist_tag
    local interleave_pages
    local dmesg_start_line=0
    local dmesg_start_boot_s=""
    mode_tag="$(printf '%s' "$cold_mode" | tr -c 'A-Za-z0-9' '_')"
    cmt_tag="$(size_tag "$cmt_label")"
    dist_tag="$(printf '%s' "$SQLITE_ACCESS_DIST" | tr -c 'A-Za-z0-9' '_')"
    if [[ "$SQLITE_INTERLEAVE_PAGES" == "window" || "$SQLITE_INTERLEAVE_PAGES" == "follow-window" ]]; then
        interleave_pages="$window_pages"
    else
        interleave_pages="$SQLITE_INTERLEAVE_PAGES"
    fi
    local wp_tag="wp${window_pages}"
    local tag="die_singlefile_pageflow_fileparallel_fullscan_${variant}_${mode_tag}_${dist_tag}_${wp_tag}_cmt_${cmt_tag}_t${threads}"
    local init_txt="${RESULT_FOLDER%/}/sqlite_die_singlefile_pageflow_fileparallel_fullscan_init_${variant}_${mode_tag}_${dist_tag}_${wp_tag}_cmt_${cmt_tag}_t${threads}.txt"
    local out_dir="${DIE_RESULT_BASE}/${variant}/${mode_tag}/${dist_tag}/${wp_tag}/cmt_${cmt_tag}/t${threads}"

    mkdir -p "$out_dir"
    dmesg_start_line="$(dmesg_line_count)"
    dmesg_start_boot_s="$(dmesg_boot_seconds)"

    echo ""
    echo "================================================================"
    echo "  [TEST1-SINGLEFILE-PAGEFLOW-FILEPARALLEL-FULLSCAN] variant=$variant  threads=$threads  cold_mode=$cold_mode  window_pages=$window_pages  cmt=$cmt_label($cmt_bytes bytes)  tag=$tag"
    echo "  single-db=ON  logical_row_bytes~32KB  est_row_pages~8  tables=$SQLITE_TABLE_COUNT  rows/tbl_override=$SQLITE_ROWS_PER_TABLE"
    echo "  target=$SQLITE_TARGET_BYTES  window_tables=$SQLITE_WINDOW_TABLES  window_pages_per_table=$window_pages  window_passes_per_round=$SQLITE_WINDOW_PASSES_PER_ROUND  interleave_pages=$interleave_pages  interleave_reads=$SQLITE_INTERLEAVE_READS  init_drop_cache_each_read=$SQLITE_INIT_DROP_CACHE_EACH_READ  analyze_latency_run=$SQLITE_ANALYZE_LATENCY_RUN  access_dist=$SQLITE_ACCESS_DIST  zipf_alpha=$ZIPF_ALPHA  normal_mean=$NORMAL_MEAN  normal_stddev=$NORMAL_STDDEV  cold_mode=$cold_mode  cold_extra_append_bytes=$SQLITE_COLD_EXTRA_APPEND_BYTES  cold_extra_mode=$SQLITE_COLD_EXTRA_MODE  cold_extra_read_ratio=$SQLITE_COLD_EXTRA_READ_RATIO  cold_extra_row_reads_per_batch=$SQLITE_COLD_EXTRA_ROW_READS_PER_BATCH  cold_extra_sleep_us=$SQLITE_COLD_EXTRA_SLEEP_US  read_sleep_every=$SQLITE_COLD_EXTRA_READ_SLEEP_EVERY  write_sleep_every=$SQLITE_COLD_EXTRA_WRITE_SLEEP_EVERY  refstyle_dummy=$SQLITE_REFSTYLE_DUMMY_BYTES  align_pages=$SQLITE_ALIGN_PAGES"
    echo "  gc_nand_timing=$SQLITE_GC_NAND_TIMING  gc_nand_timing_path=$SQLITE_GC_NAND_TIMING_PATH  bg_nand_stats_path=$SQLITE_BG_NAND_STATS_PATH  heat_epoch_path=$SQLITE_HEAT_EPOCH_PATH  test_phase_stats_path=$SQLITE_TEST_PHASE_STATS_PATH  recent_write_guard=$SQLITE_TEST_PHASE_RECENT_WRITE_GUARD  guard_read_reqs=$SQLITE_TEST_PHASE_GUARD_READ_REQS  capture_dmesg_tail=$SQLITE_CAPTURE_DMESG_TAIL  dmesg_tail_lines=$SQLITE_DMESG_TAIL_LINES  dmesg_time_slack_s=$SQLITE_DMESG_TIME_SLACK_S"
    echo "  note: default run uses no dummy; cold scan reads logical tables from one SQLite DB"
    echo "================================================================"

    load_die_module "$variant" "$cmt_bytes"
    configure_test_phase_guard_params

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
    if [[ "$SQLITE_GC_NAND_TIMING" == "1" ]]; then
        extra_args+=(--enable-gc-nand-timing)
    fi
    if [[ "$SQLITE_INIT_DROP_CACHE_EACH_READ" == "1" ]]; then
        extra_args+=(--init-drop-cache-each-read)
    fi
    if [[ "$SQLITE_REFSTYLE_DUMMY_BYTES" != "0" ]]; then
        extra_args+=(--refstyle-dummy-bytes "$SQLITE_REFSTYLE_DUMMY_BYTES")
    fi
    if [[ "$SQLITE_ALIGN_PAGES" != "0" ]]; then
        extra_args+=(--align-pages "$SQLITE_ALIGN_PAGES")
    fi

    if ! numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./${EXE_NAME} --mode init \
        --target-bytes "$SQLITE_TARGET_BYTES" \
        --table-count "$SQLITE_TABLE_COUNT" \
        --rows-per-table "$SQLITE_ROWS_PER_TABLE" \
        --window-tables "$SQLITE_WINDOW_TABLES" \
        --window-pages-per-table "$window_pages" \
        --window-passes-per-round "$SQLITE_WINDOW_PASSES_PER_ROUND" \
        --interleave-pages "$interleave_pages" \
        --interleave-reads "$SQLITE_INTERLEAVE_READS" \
        --ftl-host-page-bytes "$SQLITE_FTL_HOST_PAGE_BYTES" \
        --distribution "$SQLITE_ACCESS_DIST" \
        --zipf-seed "$ZIPF_SEED" \
        --exp-seed "$EXP_SEED" \
        --normal-seed "$NORMAL_SEED" \
        --alpha "$ZIPF_ALPHA" \
        --lambda "$EXP_LAMBDA" \
        --normal-mean "$NORMAL_MEAN" \
        --normal-stddev "$NORMAL_STDDEV" \
        --cold-full-read-mode "$cold_mode" \
        --cold-full-read-iters "$SQLITE_COLD_FULL_READ_ITERS" \
        --cold-extra-append-bytes "$SQLITE_COLD_EXTRA_APPEND_BYTES" \
        --cold-extra-mode "$SQLITE_COLD_EXTRA_MODE" \
        --cold-extra-read-ratio "$SQLITE_COLD_EXTRA_READ_RATIO" \
        --cold-extra-row-reads-per-batch "$SQLITE_COLD_EXTRA_ROW_READS_PER_BATCH" \
        --cold-extra-sleep-us "$SQLITE_COLD_EXTRA_SLEEP_US" \
        --cold-extra-read-sleep-every "$SQLITE_COLD_EXTRA_READ_SLEEP_EVERY" \
        --cold-extra-write-sleep-every "$SQLITE_COLD_EXTRA_WRITE_SLEEP_EVERY" \
	        --cold-concurrent-threads "$threads" \
	        --test-phase-path "$SQLITE_TEST_PHASE_PATH" \
	        --test-phase-stats-path "$SQLITE_TEST_PHASE_STATS_PATH" \
	        --heat-epoch-path "$SQLITE_HEAT_EPOCH_PATH" \
	        --gc-nand-timing-path "$SQLITE_GC_NAND_TIMING_PATH" \
        --bg-nand-stats-path "$SQLITE_BG_NAND_STATS_PATH" \
        --strict-cold-per-select \
        "${extra_args[@]}" \
        --tag "$tag" \
        >"$init_txt" 2>&1; then
        echo "ERROR: workload failed for variant=$variant threads=$threads" >&2
        capture_kernel_tail "$tag" "$init_txt" "$out_dir" "$dmesg_start_line" "$dmesg_start_boot_s"
        print_init_log_tail "$init_txt"
        return 1
    fi

    if ! validate_workload_outputs "$tag" "$init_txt" "$cold_mode"; then
        echo "ERROR: workload exited without producing expected outputs for variant=$variant threads=$threads" >&2
        capture_kernel_tail "$tag" "$init_txt" "$out_dir" "$dmesg_start_line" "$dmesg_start_boot_s"
        print_init_log_tail "$init_txt"
        return 1
    fi

    cp "$init_txt" "${out_dir}/" 2>/dev/null || true
    cp "${RESULT_FOLDER}"/sqlite_table_tier_${tag}.csv "${out_dir}/" 2>/dev/null || true
    cp "${RESULT_FOLDER}"/sqlite_page_tier_${tag}.csv  "${out_dir}/" 2>/dev/null || true
    cp "${RESULT_FOLDER}"/sqlite_table_die_${tag}.csv  "${out_dir}/" 2>/dev/null || true
    cp "${RESULT_FOLDER}"/sqlite_table_chain_${tag}.csv "${out_dir}/" 2>/dev/null || true
    cp "${RESULT_FOLDER}"/sqlite_table_chain_die_${tag}.csv "${out_dir}/" 2>/dev/null || true
    cp "${RESULT_FOLDER}"/sqlite_stream_die_stats_${tag}.csv "${out_dir}/" 2>/dev/null || true
    cp "${RESULT_FOLDER}"/sqlite_table_${tag}.csv      "${out_dir}/" 2>/dev/null || true
    cp "${RESULT_FOLDER}"/sqlite_row_${tag}.csv        "${out_dir}/" 2>/dev/null || true
    cp "${RESULT_FOLDER}"/sqlite_bg_nand_phase_${tag}.csv "${out_dir}/" 2>/dev/null || true
    if [[ -r "$SQLITE_DIE_AFFINITY_STATS_PATH" ]]; then
        {
            echo "[die_affinity_stats]"
            cat "$SQLITE_DIE_AFFINITY_STATS_PATH"
        } >>"$init_txt" 2>/dev/null || true
        cp "$SQLITE_DIE_AFFINITY_STATS_PATH" \
           "${RESULT_FOLDER%/}/sqlite_die_affinity_stats_${tag}.txt" 2>/dev/null || true
        cp "${RESULT_FOLDER%/}/sqlite_die_affinity_stats_${tag}.txt" "${out_dir}/" 2>/dev/null || true
    fi
    if [[ -r "$SQLITE_LPN_DIE_CHANGE_STATS_PATH" ]]; then
        {
            echo "[lpn_die_change_stats]"
            cat "$SQLITE_LPN_DIE_CHANGE_STATS_PATH"
        } >>"$init_txt" 2>/dev/null || true
        cp "$SQLITE_LPN_DIE_CHANGE_STATS_PATH" \
           "${RESULT_FOLDER%/}/sqlite_lpn_die_change_stats_${tag}.txt" 2>/dev/null || true
        cp "${RESULT_FOLDER%/}/sqlite_lpn_die_change_stats_${tag}.txt" "${out_dir}/" 2>/dev/null || true
    fi
    if [[ -r "$SQLITE_TEST_PHASE_STATS_PATH" ]]; then
        {
            echo "[test_phase_stats]"
            cat "$SQLITE_TEST_PHASE_STATS_PATH"
        } >>"$init_txt" 2>/dev/null || true
        cp "$SQLITE_TEST_PHASE_STATS_PATH" \
           "${RESULT_FOLDER%/}/sqlite_test_phase_stats_${tag}.txt" 2>/dev/null || true
        cp "${RESULT_FOLDER%/}/sqlite_test_phase_stats_${tag}.txt" "${out_dir}/" 2>/dev/null || true
        if write_numeric_stats_aggregate "$SQLITE_TEST_PHASE_STATS_PATH" \
            "${RESULT_FOLDER%/}/sqlite_test_phase_stats_aggregate_${tag}.txt"; then
            {
                echo "[test_phase_stats_aggregate]"
                cat "${RESULT_FOLDER%/}/sqlite_test_phase_stats_aggregate_${tag}.txt"
            } >>"$init_txt" 2>/dev/null || true
            cp "${RESULT_FOLDER%/}/sqlite_test_phase_stats_aggregate_${tag}.txt" \
               "${out_dir}/" 2>/dev/null || true
        fi
    fi
    if [[ -r "$SQLITE_SUPERBLOCK_STATS_PATH" ]]; then
        {
            echo "[superblock_stats]"
            cat "$SQLITE_SUPERBLOCK_STATS_PATH"
        } >>"$init_txt" 2>/dev/null || true
        cp "$SQLITE_SUPERBLOCK_STATS_PATH" \
           "${RESULT_FOLDER%/}/sqlite_superblock_stats_${tag}.txt" 2>/dev/null || true
        cp "${RESULT_FOLDER%/}/sqlite_superblock_stats_${tag}.txt" "${out_dir}/" 2>/dev/null || true
    fi
    if [[ -r "$SQLITE_DIE_STATS_PATH" ]]; then
        {
            echo "[die_stats]"
            cat "$SQLITE_DIE_STATS_PATH"
        } >>"$init_txt" 2>/dev/null || true
        cp "$SQLITE_DIE_STATS_PATH" \
           "${RESULT_FOLDER%/}/sqlite_die_stats_${tag}.txt" 2>/dev/null || true
        cp "${RESULT_FOLDER%/}/sqlite_die_stats_${tag}.txt" "${out_dir}/" 2>/dev/null || true
    fi
    if [[ -r "$SQLITE_BG_NAND_STATS_PATH" ]]; then
        {
            echo "[bg_nand_stats]"
            cat "$SQLITE_BG_NAND_STATS_PATH"
        } >>"$init_txt" 2>/dev/null || true
        cp "$SQLITE_BG_NAND_STATS_PATH" \
           "${RESULT_FOLDER%/}/sqlite_bg_nand_stats_${tag}.txt" 2>/dev/null || true
        cp "${RESULT_FOLDER%/}/sqlite_bg_nand_stats_${tag}.txt" "${out_dir}/" 2>/dev/null || true
    fi
    capture_kernel_tail "$tag" "$init_txt" "$out_dir" "$dmesg_start_line" "$dmesg_start_boot_s"

    if [[ "$SQLITE_ANALYZE_LATENCY_RUN" == "1" && -r "$SCRIPT_DIR/sqlite/analyze_latency_run.py" ]]; then
        local summary_file="${RESULT_FOLDER%/}/sqlite_latency_summary_${tag}.txt"

        if python3 "$SCRIPT_DIR/sqlite/analyze_latency_run.py" "$init_txt" >"$summary_file" 2>/dev/null; then
            {
                echo "[latency_summary]"
                cat "$summary_file"
            } >>"$init_txt" 2>/dev/null || true
            echo "[latency_summary] $tag"
            cat "$summary_file"
            cp "$summary_file" "${out_dir}/" 2>/dev/null || true
        else
            echo "[latency_summary] failed to analyze $init_txt" >>"$init_txt" 2>/dev/null || true
            echo "[latency_summary] failed to analyze $init_txt"
        fi
    fi
    cp "$init_txt" "${out_dir}/" 2>/dev/null || true

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
        for access_dist in $SQLITE_ACCESS_DIST_LIST; do
            SQLITE_ACCESS_DIST="$access_dist"
            for cmt_label in $SQLITE_MAP_CMT_BYTES_LIST; do
                cmt_bytes="$(size_to_bytes "$cmt_label")" || exit 1
                for cold_mode in $SQLITE_COLD_FULL_READ_MODE; do
                    for window_pages in $SQLITE_WINDOW_PAGES_PER_TABLE_LIST; do
                        run_one_test "$variant" "$threads" "$cold_mode" "$cmt_label" "$cmt_bytes" "$window_pages" || exit 1
                    done
                done
            done
        done
    done
done

./enablemeta.sh

echo ""
    echo "========================================"
echo "  [TEST1-SINGLEFILE-PAGEFLOW-FILEPARALLEL-FULLSCAN] All tests completed."
echo "  Results in: $DIE_RESULT_BASE"
echo "========================================"
