#!/bin/bash
set -euo pipefail
#
# fio_fragment_die_latency_mixed.sh
#
# FIO counterpart to the SQLite latency-maintenance experiment:
#   - init writes an 8 GiB raw-device region
#   - warmup reads that initialized region
#   - measured phase runs mixed random reads/overwrites with read:write ratios
#     such as 10:1, 8:2, and 7:3
#   - access distribution is swept across zipf and normal by default
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

source commonvariables.sh

NVMEV_DIR="${SCRIPT_DIR}/../nvmevirt_DA"

# Match the current SQLite comparison set by default.
VARIANTS="${VARIANTS:-die_latency1_qlc_all_norp_sb die_latency2_qlc_all_norp_sb die_latency3_qlc_all_norp_sb die_latency1_sb}"

FIO_JOB_FILE="${FIO_JOB_FILE:-fio_latency_v2_mixed.fio}"
FIO_ACCESS_DIST_LIST="${FIO_ACCESS_DIST_LIST:-${SQLITE_ACCESS_DIST_LIST:-zipf normal}}"
FIO_RW_RATIOS="${FIO_RW_RATIOS:-10:1 8:2 7:3}"
FIO_REGION_SIZE="${FIO_REGION_SIZE:-8G}"
FIO_INIT_WRITE_BYTES="${FIO_INIT_WRITE_BYTES:-$FIO_REGION_SIZE}"
FIO_PREWARM_SEQ_BYTES="${FIO_PREWARM_SEQ_BYTES:-$FIO_REGION_SIZE}"
FIO_PREWARM_RANDOM_BYTES="${FIO_PREWARM_RANDOM_BYTES:-16G}"
FIO_MEASURE_TOTAL_BYTES="${FIO_MEASURE_TOTAL_BYTES:-88G}"
FIO_PREWARM_JOBS="${FIO_PREWARM_JOBS:-2}"
FIO_PREWARM_IODEPTH="${FIO_PREWARM_IODEPTH:-32}"
FIO_MEASURE_JOBS="${FIO_MEASURE_JOBS:-${FIO_READ_JOBS:-4}}"
FIO_MEASURE_IODEPTH="${FIO_MEASURE_IODEPTH:-${FIO_READ_IODEPTH:-32}}"
FIO_MEASURE_RATE="${FIO_MEASURE_RATE:-}"
FIO_MEASURE_RATE_IOPS="${FIO_MEASURE_RATE_IOPS:-}"
FIO_ZIPF_ALPHA="${FIO_ZIPF_ALPHA:-${ZIPF_ALPHA:-0.75}}"
FIO_NORMAL_DEVIATION="${FIO_NORMAL_DEVIATION:-20}"
FIO_REBUILD_DIE_MODULES="${FIO_REBUILD_DIE_MODULES:-${SQLITE_REBUILD_DIE_MODULES:-0}}"
FIO_DRY_RUN="${FIO_DRY_RUN:-0}"
FIO_OUTPUT_DIR="${FIO_OUTPUT_DIR:-${RESULT_FOLDER%/}/fio_latency_mixed}"

FIO_TEST_PHASE_PATH="${FIO_TEST_PHASE_PATH:-/sys/kernel/debug/nvmev/ftl0/test_phase}"
FIO_TEST_PHASE_STATS_PATH="${FIO_TEST_PHASE_STATS_PATH:-/sys/kernel/debug/nvmev/ftl0/test_phase_stats}"
FIO_SUPERBLOCK_STATS_PATH="${FIO_SUPERBLOCK_STATS_PATH:-/sys/kernel/debug/nvmev/ftl0/superblock_stats}"
FIO_DIE_STATS_PATH="${FIO_DIE_STATS_PATH:-/sys/module/nvmev/parameters/die_stats}"
FIO_BG_NAND_STATS_PATH="${FIO_BG_NAND_STATS_PATH:-/sys/module/nvmev/parameters/bg_nand_stats}"
FIO_GC_NAND_TIMING="${FIO_GC_NAND_TIMING:-${SQLITE_GC_NAND_TIMING:-1}}"
FIO_GC_NAND_TIMING_PATH="${FIO_GC_NAND_TIMING_PATH:-/sys/module/nvmev/parameters/gc_nand_timing}"
FIO_TEST_PHASE_RECENT_WRITE_GUARD="${FIO_TEST_PHASE_RECENT_WRITE_GUARD:-${SQLITE_TEST_PHASE_RECENT_WRITE_GUARD:-1}}"
FIO_TEST_PHASE_GUARD_READ_REQS="${FIO_TEST_PHASE_GUARD_READ_REQS:-${SQLITE_TEST_PHASE_GUARD_READ_REQS:-256}}"
FIO_TEST_PHASE_RECENT_WRITE_GUARD_PATH="${FIO_TEST_PHASE_RECENT_WRITE_GUARD_PATH:-/sys/module/nvmev/parameters/test_phase_recent_write_guard}"
FIO_TEST_PHASE_GUARD_READ_REQS_PATH="${FIO_TEST_PHASE_GUARD_READ_REQS_PATH:-/sys/module/nvmev/parameters/test_phase_guard_read_reqs}"

mkdir -p "$RESULT_FOLDER" "$FIO_OUTPUT_DIR"

die() {
    echo "ERROR: $*" >&2
    exit 1
}

size_tag() {
    printf '%s' "$1" | tr -c 'A-Za-z0-9' '_'
}

ratio_tag() {
    printf '%s' "$1" | tr ':' '_' | tr -c 'A-Za-z0-9_' '_'
}

dist_tag() {
    printf '%s' "$1" | tr ':' '_' | tr -c 'A-Za-z0-9_' '_'
}

ratio_to_mixread() {
    local ratio="$1"
    local read_part
    local write_part

    if [[ "$ratio" =~ ^([0-9]+)$ ]]; then
        if [[ "${BASH_REMATCH[1]}" -le 0 || "${BASH_REMATCH[1]}" -ge 100 ]]; then
            die "FIO_RW_RATIOS percentage '$ratio' must be 1..99"
        fi
        echo "${BASH_REMATCH[1]}"
        return 0
    fi

    if [[ ! "$ratio" =~ ^([0-9]+):([0-9]+)$ ]]; then
        die "invalid FIO_RW_RATIOS entry '$ratio'; use read:write, e.g. 10:1"
    fi

    read_part="${BASH_REMATCH[1]}"
    write_part="${BASH_REMATCH[2]}"
    awk -v r="$read_part" -v w="$write_part" '
        BEGIN {
            if (r <= 0 || w <= 0) exit 1
            pct = int((100.0 * r / (r + w)) + 0.5)
            if (pct < 1) pct = 1
            if (pct > 99) pct = 99
            print pct
        }
    ' || die "invalid ratio '$ratio'"
}

dist_to_fio() {
    local dist="$1"

    case "$dist" in
        zipf)
            echo "zipf:${FIO_ZIPF_ALPHA}"
            ;;
        normal)
            if [[ -n "$FIO_NORMAL_DEVIATION" ]]; then
                echo "normal:${FIO_NORMAL_DEVIATION}"
            else
                echo "normal"
            fi
            ;;
        uniform)
            echo "random"
            ;;
        *)
            # Allows explicit fio syntax, e.g. zipf:0.75 or normal:15.
            echo "$dist"
            ;;
    esac
}

drop_caches() {
    sync
    echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
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

configure_module_params() {
    set_optional_module_param "$FIO_GC_NAND_TIMING_PATH" \
        "$FIO_GC_NAND_TIMING" "gc_nand_timing"
    set_optional_module_param "$FIO_TEST_PHASE_RECENT_WRITE_GUARD_PATH" \
        "$FIO_TEST_PHASE_RECENT_WRITE_GUARD" "test_phase_recent_write_guard"
    set_optional_module_param "$FIO_TEST_PHASE_GUARD_READ_REQS_PATH" \
        "$FIO_TEST_PHASE_GUARD_READ_REQS" "test_phase_guard_read_reqs"
}

set_test_phase() {
    local value="$1"

    if [[ ! -e "$FIO_TEST_PHASE_PATH" ]]; then
        return 1
    fi
    echo "$value" | sudo tee "$FIO_TEST_PHASE_PATH" >/dev/null
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
    elif [[ -e "$path" ]]; then
        printf '%s\n' "$path"
    fi
}

capture_stat_file() {
    local path="$1"
    local name="$2"
    local tag="$3"
    local out_dir="$4"
    local out_file="${FIO_OUTPUT_DIR%/}/${name}_${tag}.txt"

    [[ -e "$path" ]] || return 0
    if sudo cat "$path" >"$out_file" 2>/dev/null; then
        cp "$out_file" "$out_dir/" 2>/dev/null || true
    fi
}

capture_numeric_stats_aggregate() {
    local path="$1"
    local name="$2"
    local tag="$3"
    local out_dir="$4"
    local out_file="${FIO_OUTPUT_DIR%/}/${name}_aggregate_${tag}.txt"
    local files=()

    mapfile -t files < <(debugfs_sibling_files "$path")
    [[ ${#files[@]} -gt 0 ]] || return 0

    if sudo cat "${files[@]}" 2>/dev/null | awk '
        NF == 2 && $2 ~ /^[0-9]+$/ {
            if (!seen[$1]++) order[++n] = $1
            sum[$1] += $2
        }
        END {
            for (i = 1; i <= n; i++) print order[i], sum[order[i]]
        }
    ' >"$out_file"; then
        cp "$out_file" "$out_dir/" 2>/dev/null || true
    fi
}

capture_run_stats() {
    local tag="$1"
    local out_dir="$2"

    capture_stat_file "$FIO_TEST_PHASE_STATS_PATH" "fio_test_phase_stats" "$tag" "$out_dir"
    capture_numeric_stats_aggregate "$FIO_TEST_PHASE_STATS_PATH" "fio_test_phase_stats" "$tag" "$out_dir"
    capture_stat_file "$FIO_SUPERBLOCK_STATS_PATH" "fio_superblock_stats" "$tag" "$out_dir"
    capture_stat_file "$FIO_DIE_STATS_PATH" "fio_die_stats" "$tag" "$out_dir"
    capture_stat_file "$FIO_BG_NAND_STATS_PATH" "fio_bg_nand_stats" "$tag" "$out_dir"
}

load_variant_module() {
    local variant="$1"
    local ko_path="${NVMEV_DIR}/nvmev_${variant}.ko"

    [[ -f "$ko_path" ]] || die "$ko_path not found. Build it first or set FIO_REBUILD_DIE_MODULES=1."

    echo "=== Loading $variant for fio ==="
    if [[ -f ./nvmev_on.ko ]]; then
        cp ./nvmev_on.ko ./nvmev_on.ko.fio_bak
    fi
    cp "$ko_path" ./nvmev_on.ko
    ./nvmevstart_on.sh
    if [[ -f ./nvmev_on.ko.fio_bak ]]; then
        mv ./nvmev_on.ko.fio_bak ./nvmev_on.ko
    fi
    sleep 1
}

render_fio_job() {
    local out_job="$1"
    local fio_dist="$2"
    local mixread="$3"
    local log_prefix="$4"
    local test_phase_hook_path="$5"

    awk \
        -v filename="$DATA_DEV" \
        -v region_size="$FIO_REGION_SIZE" \
        -v init_write_bytes="$FIO_INIT_WRITE_BYTES" \
        -v prewarm_seq_bytes="$FIO_PREWARM_SEQ_BYTES" \
        -v prewarm_random_bytes="$FIO_PREWARM_RANDOM_BYTES" \
        -v prewarm_jobs="$FIO_PREWARM_JOBS" \
        -v prewarm_iodepth="$FIO_PREWARM_IODEPTH" \
        -v measure_total_bytes="$FIO_MEASURE_TOTAL_BYTES" \
        -v measure_jobs="$FIO_MEASURE_JOBS" \
        -v measure_iodepth="$FIO_MEASURE_IODEPTH" \
        -v fio_dist="$fio_dist" \
        -v mixread="$mixread" \
        -v log_prefix="$log_prefix" \
        -v test_phase_hook_path="$test_phase_hook_path" \
        -v measure_rate="$FIO_MEASURE_RATE" \
        -v measure_rate_iops="$FIO_MEASURE_RATE_IOPS" '
        function emit_enter_hook() {
            if (section == "enter_test_phase" && !enter_hook_done) {
                if (test_phase_hook_path != "")
                    print "exec_prerun=echo 1 > " test_phase_hook_path
                enter_hook_done = 1
            }
        }
        function emit_measure_extras() {
            if (section == "measure_mixed" && !measure_extra_done) {
                if (measure_rate != "") print "rate=" measure_rate
                if (measure_rate_iops != "") print "rate_iops=" measure_rate_iops
                measure_extra_done = 1
            }
        }
        /^\[/ {
            emit_enter_hook()
            emit_measure_extras()
            section = $0
            gsub(/^\[/, "", section)
            gsub(/\]$/, "", section)
            print
            next
        }
        section == "global" && /^filename=/ {
            print "filename=" filename
            next
        }
        section == "init_write_8g" && /^size=/ {
            print "size=" region_size
            next
        }
        section == "init_write_8g" && /^io_size=/ {
            print "io_size=" init_write_bytes
            next
        }
        section == "prewarm_seq_read" && /^size=/ {
            print "size=" region_size
            next
        }
        section == "prewarm_seq_read" && /^io_size=/ {
            print "io_size=" prewarm_seq_bytes
            next
        }
        section == "prewarm_dist_read" && /^size=/ {
            print "size=" region_size
            next
        }
        section == "prewarm_dist_read" && /^io_size=/ {
            print "io_size=" prewarm_random_bytes
            next
        }
        section == "prewarm_dist_read" && /^random_distribution=/ {
            print "random_distribution=" fio_dist
            next
        }
        section == "prewarm_dist_read" && /^numjobs=/ {
            print "numjobs=" prewarm_jobs
            next
        }
        section == "prewarm_dist_read" && /^iodepth=/ {
            print "iodepth=" prewarm_iodepth
            next
        }
        section == "measure_mixed" && /^size=/ {
            print "size=" region_size
            next
        }
        section == "measure_mixed" && /^io_size=/ {
            print "io_size=" measure_total_bytes
            next
        }
        section == "measure_mixed" && /^random_distribution=/ {
            print "random_distribution=" fio_dist
            next
        }
        section == "measure_mixed" && /^rwmixread=/ {
            print "rwmixread=" mixread
            next
        }
        section == "measure_mixed" && /^numjobs=/ {
            print "numjobs=" measure_jobs
            next
        }
        section == "measure_mixed" && /^iodepth=/ {
            print "iodepth=" measure_iodepth
            next
        }
        section == "measure_mixed" && /^write_lat_log=/ {
            print "write_lat_log=" log_prefix
            next
        }
        section == "measure_mixed" && /^write_bw_log=/ {
            print "write_bw_log=" log_prefix
            next
        }
        section == "measure_mixed" && /^write_iops_log=/ {
            print "write_iops_log=" log_prefix
            next
        }
        section == "measure_mixed" && /^(rate|rate_iops)=/ {
            next
        }
        section == "enter_test_phase" && /^exec_prerun=/ {
            next
        }
        { print }
        END {
            emit_enter_hook()
            emit_measure_extras()
        }
    ' "$FIO_JOB_FILE" >"$out_job"
}

run_fio_one_case() {
    local variant="$1"
    local access_dist="$2"
    local ratio="$3"
    local mixread
    local fio_dist
    local ts
    local tag
    local out_dir
    local out_json
    local fio_job_tmp
    local fio_job_saved
    local log_prefix
    local test_phase_hook_path=""

    mixread="$(ratio_to_mixread "$ratio")"
    fio_dist="$(dist_to_fio "$access_dist")"
    ts="$(date +%Y%m%d_%H%M%S)"
    tag="fio_mixed_${variant}_$(dist_tag "$access_dist")_rw$(ratio_tag "$ratio")_${ts}"
    out_dir="${FIO_OUTPUT_DIR%/}/${variant}/$(dist_tag "$access_dist")/rw$(ratio_tag "$ratio")"
    out_json="${out_dir}/${tag}.json"
    fio_job_tmp="$(mktemp "${TMPDIR:-/tmp}/fio_latency_v2_mixed_${variant}.XXXXXX.fio")"
    fio_job_saved="${out_dir}/${tag}.fio"
    log_prefix="${out_dir}/${tag}"

    mkdir -p "$out_dir"

    echo ""
    echo "================================================================"
    echo "  [FIO-LATENCY-MIXED] variant=$variant dist=$access_dist($fio_dist) ratio=$ratio rwmixread=$mixread"
    echo "  region=$FIO_REGION_SIZE init_write=$FIO_INIT_WRITE_BYTES prewarm_seq=$FIO_PREWARM_SEQ_BYTES prewarm_random=$FIO_PREWARM_RANDOM_BYTES measure_total=$FIO_MEASURE_TOTAL_BYTES"
    echo "  measure_jobs=$FIO_MEASURE_JOBS measure_iodepth=$FIO_MEASURE_IODEPTH gc_nand_timing=$FIO_GC_NAND_TIMING recent_write_guard=$FIO_TEST_PHASE_RECENT_WRITE_GUARD guard_read_reqs=$FIO_TEST_PHASE_GUARD_READ_REQS"
    echo "================================================================"

    if [[ "$FIO_DRY_RUN" != "1" ]]; then
        load_variant_module "$variant"
        configure_module_params
        lsblk
        echo 0 | sudo tee /sys/block/${DATA_NAME}/queue/read_ahead_kb >/dev/null 2>&1 || true
        drop_caches
        set_test_phase 0 || true
        if [[ -e "$FIO_TEST_PHASE_PATH" ]]; then
            test_phase_hook_path="$FIO_TEST_PHASE_PATH"
        else
            echo "[test_phase] $FIO_TEST_PHASE_PATH not present; generated job will run without test_phase hook" >&2
        fi
    else
        test_phase_hook_path="$FIO_TEST_PHASE_PATH"
    fi

    render_fio_job "$fio_job_tmp" "$fio_dist" "$mixread" "$log_prefix" "$test_phase_hook_path"
    cp "$fio_job_tmp" "$fio_job_saved"

    echo "=== fio jobfile: $fio_job_saved ==="
    if [[ "$FIO_DRY_RUN" == "1" ]]; then
        rm -f "$fio_job_tmp"
        return 0
    fi

    if sudo fio "$fio_job_tmp" --output="$out_json" --output-format=json; then
        set_test_phase 0 || true
        capture_run_stats "$tag" "$out_dir"
    else
        local rc=$?
        set_test_phase 0 || true
        capture_run_stats "$tag" "$out_dir"
        rm -f "$fio_job_tmp"
        sudo rmmod nvmev 2>/dev/null || rmmod nvmev 2>/dev/null || true
        sleep 5
        return "$rc"
    fi

    rm -f "$fio_job_tmp"
    sudo rmmod nvmev 2>/dev/null || rmmod nvmev 2>/dev/null || true
    sleep 5
}

if [[ ! -f "$FIO_JOB_FILE" ]]; then
    die "FIO_JOB_FILE '$FIO_JOB_FILE' not found"
fi

if [[ "$FIO_DRY_RUN" != "1" ]] && ! command -v fio >/dev/null 2>&1; then
    die "fio is required but was not found in PATH"
fi

if [[ "$FIO_REBUILD_DIE_MODULES" == "1" ]]; then
    echo "=== Rebuilding die modules for fio: $VARIANTS ==="
    (cd "$NVMEV_DIR" && bash build_die.sh $VARIANTS)
fi

for variant in $VARIANTS; do
    for access_dist in $FIO_ACCESS_DIST_LIST; do
        for ratio in $FIO_RW_RATIOS; do
            run_fio_one_case "$variant" "$access_dist" "$ratio"
        done
    done
done

echo ""
echo "========================================"
echo "  [FIO-LATENCY-MIXED] All tests completed."
echo "  Results in: $FIO_OUTPUT_DIR"
echo "========================================"
