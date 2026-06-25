#!/bin/bash
set -euo pipefail
#
# fio_fragment_die_latency_mixed.sh
#
# FIO counterpart to the SQLite latency-maintenance experiment:
#   - init writes an 8 GiB raw-device region in 512 MiB chunks by default
#   - each just-written chunk is distribution-read before the next write chunk
#   - measured phase runs mixed random reads/overwrites with read:write ratios
#     such as 10:1, 8:2, and 7:3
#   - access distribution is swept across zipf and normal by default
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

source commonvariables.sh

NVMEV_DIR="${SCRIPT_DIR}/../nvmevirt_DA"

# Match the current placement/read-priority comparison set by default.
VARIANTS="${VARIANTS:-die_latency1_qlc_all_norp_sb die_latency2_qlc_all_norp_sb die_latency3_qlc_all_norp_sb die_latency1_norp_sb}"

FIO_JOB_FILE="${FIO_JOB_FILE:-fio_latency_v2_mixed.fio}"
FIO_ACCESS_DIST_LIST="${FIO_ACCESS_DIST_LIST:-${SQLITE_ACCESS_DIST_LIST:-zipf normal}}"
FIO_RW_RATIOS="${FIO_RW_RATIOS:-10:1 8:2 7:3}"
FIO_REGION_SIZE="${FIO_REGION_SIZE:-8G}"
FIO_INIT_WRITE_BYTES="${FIO_INIT_WRITE_BYTES:-$FIO_REGION_SIZE}"
FIO_INIT_CHUNK_SIZE="${FIO_INIT_CHUNK_SIZE:-512M}"
FIO_INIT_CHUNK_COUNT="${FIO_INIT_CHUNK_COUNT:-0}"
FIO_INIT_PREWARM_BYTES_PER_CHUNK="${FIO_INIT_PREWARM_BYTES_PER_CHUNK:-${FIO_INIT_PREWARM_READ_BYTES:-5x}}"
FIO_INIT_PREWARM_GUARD="${FIO_INIT_PREWARM_GUARD:-1}"
FIO_PREWARM_SEQ_BYTES="${FIO_PREWARM_SEQ_BYTES:-0}"
FIO_PREWARM_RANDOM_BYTES="${FIO_PREWARM_RANDOM_BYTES:-0}"
FIO_MEASURE_TOTAL_BYTES="${FIO_MEASURE_TOTAL_BYTES:-88G}"
FIO_MEASURE_READ_BYTES="${FIO_MEASURE_READ_BYTES:-}"
FIO_MEASURE_WRITE_BYTES="${FIO_MEASURE_WRITE_BYTES:-5G}"
FIO_MEASURE_WRITE_OFFSET="${FIO_MEASURE_WRITE_OFFSET:-$FIO_REGION_SIZE}"
FIO_MEASURE_WRITE_REGION_SIZE="${FIO_MEASURE_WRITE_REGION_SIZE:-io_size}"
FIO_PREWARM_JOBS="${FIO_PREWARM_JOBS:-2}"
FIO_PREWARM_IODEPTH="${FIO_PREWARM_IODEPTH:-32}"
FIO_INIT_PREWARM_JOBS="${FIO_INIT_PREWARM_JOBS:-1}"
FIO_INIT_PREWARM_IODEPTH="${FIO_INIT_PREWARM_IODEPTH:-32}"
FIO_MEASURE_JOBS="${FIO_MEASURE_JOBS:-${FIO_READ_JOBS:-1}}"
FIO_MEASURE_READ_JOBS="${FIO_MEASURE_READ_JOBS:-$FIO_MEASURE_JOBS}"
FIO_MEASURE_WRITE_JOBS="${FIO_MEASURE_WRITE_JOBS:-$FIO_MEASURE_JOBS}"
FIO_MEASURE_IODEPTH="${FIO_MEASURE_IODEPTH:-${FIO_READ_IODEPTH:-32}}"
FIO_MEASURE_READ_IODEPTH="${FIO_MEASURE_READ_IODEPTH:-$FIO_MEASURE_IODEPTH}"
FIO_MEASURE_WRITE_IODEPTH="${FIO_MEASURE_WRITE_IODEPTH:-$FIO_MEASURE_IODEPTH}"
FIO_MEASURE_RATE="${FIO_MEASURE_RATE:-}"
FIO_MEASURE_RATE_IOPS="${FIO_MEASURE_RATE_IOPS:-}"
FIO_ZIPF_ALPHA="${FIO_ZIPF_ALPHA:-${ZIPF_ALPHA:-0.75}}"
FIO_NORMAL_DEVIATION="${FIO_NORMAL_DEVIATION:-20}"
FIO_REBUILD_DIE_MODULES="${FIO_REBUILD_DIE_MODULES:-${SQLITE_REBUILD_DIE_MODULES:-0}}"
FIO_DRY_RUN="${FIO_DRY_RUN:-0}"
FIO_OUTPUT_DIR="${FIO_OUTPUT_DIR:-${RESULT_FOLDER%/}/fio_latency_mixed}"
FIO_DRAIN_AFTER_MEASURE="${FIO_DRAIN_AFTER_MEASURE:-0}"
FIO_DRAIN_MIN_SEC="${FIO_DRAIN_MIN_SEC:-60}"
FIO_DRAIN_TIMEOUT_SEC="${FIO_DRAIN_TIMEOUT_SEC:-300}"
FIO_DRAIN_POLL_SEC="${FIO_DRAIN_POLL_SEC:-5}"
FIO_DRAIN_STABLE_POLLS="${FIO_DRAIN_STABLE_POLLS:-3}"
FIO_DRAIN_STABLE_KEYS="${FIO_DRAIN_STABLE_KEYS:-bg_repromote_ops bg_qlc_rebalance_ops slc_to_qlc_migration_pages slc_gc_valid_copy_pages slc_gc_invalid_reclaimed_pages qlc_repromote_pages internal_write_pages_est slc_to_qlc_nand_reads slc_to_qlc_nand_writes repromote_nand_reads repromote_nand_writes}"
FIO_STRICT_COMPILE_FLAGS="${FIO_STRICT_COMPILE_FLAGS:-1}"
FIO_REPEAT_COUNT="${FIO_REPEAT_COUNT:-3}"
FIO_RANDSEED_BASE="${FIO_RANDSEED_BASE:-240622}"

FIO_TEST_PHASE_PATH="${FIO_TEST_PHASE_PATH:-/sys/kernel/debug/nvmev/ftl0/test_phase}"
FIO_HEAT_EPOCH_PATH="${FIO_HEAT_EPOCH_PATH:-/sys/kernel/debug/nvmev/ftl0/heat_epoch}"
FIO_INIT_ADVANCE_HEAT_EPOCH_EACH_CHUNK="${FIO_INIT_ADVANCE_HEAT_EPOCH_EACH_CHUNK:-1}"
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

size_to_bytes() {
    local raw="$1"
    local num unit

    if [[ "$raw" =~ ^([0-9]+)([KkMmGgTt]?)$ ]]; then
        num="${BASH_REMATCH[1]}"
        unit="${BASH_REMATCH[2]}"
    else
        die "invalid size '$raw'; use integer bytes or K/M/G/T suffix"
    fi

    case "$unit" in
        K|k) echo $((num * 1024)) ;;
        M|m) echo $((num * 1024 * 1024)) ;;
        G|g) echo $((num * 1024 * 1024 * 1024)) ;;
        T|t) echo $((num * 1024 * 1024 * 1024 * 1024)) ;;
        *) echo "$num" ;;
    esac
}

is_disabled_size() {
    case "$1" in
        0|0K|0k|0M|0m|0G|0g|0T|0t|off|none|skip)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

fio_size_end() {
    local offset_bytes="$1"
    local range_bytes="$2"

    echo $((offset_bytes + range_bytes))
}

ratio_tag() {
    sanitize_tag "$1"
}

sanitize_tag() {
    printf '%s' "$1" | tr ':' '_' | tr -c 'A-Za-z0-9_' '_'
}

dist_tag() {
    local dist="$1"

    case "$dist" in
        zipf)
            sanitize_tag "zipf_a${FIO_ZIPF_ALPHA}"
            ;;
        zipf:*)
            sanitize_tag "zipf_a${dist#zipf:}"
            ;;
        normal)
            if [[ -n "$FIO_NORMAL_DEVIATION" ]]; then
                sanitize_tag "normal_d${FIO_NORMAL_DEVIATION}"
            else
                sanitize_tag "normal"
            fi
            ;;
        normal:*)
            sanitize_tag "normal_d${dist#normal:}"
            ;;
        *)
            sanitize_tag "$dist"
            ;;
    esac
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

ratio_to_measure_bytes() {
    local ratio="$1"
    local total_bytes
    local total_pages
    local configured_read_bytes
    local configured_write_bytes
    local read_part
    local write_part
    local read_pages
    local write_pages

    if [[ "$ratio" =~ ^([0-9]+)$ ]]; then
        read_part="${BASH_REMATCH[1]}"
        write_part=$((100 - read_part))
    elif [[ "$ratio" =~ ^([0-9]+):([0-9]+)$ ]]; then
        read_part="${BASH_REMATCH[1]}"
        write_part="${BASH_REMATCH[2]}"
    else
        die "invalid FIO_RW_RATIOS entry '$ratio'; use read:write, e.g. 10:1"
    fi

    if [[ -n "$FIO_MEASURE_READ_BYTES" ]]; then
        configured_read_bytes="$(size_to_bytes "$FIO_MEASURE_READ_BYTES")"
        (( configured_read_bytes % 4096 == 0 )) || die "FIO_MEASURE_READ_BYTES must be 4K aligned"
        read_pages=$((configured_read_bytes / 4096))
    fi
    if [[ -n "$FIO_MEASURE_WRITE_BYTES" ]]; then
        configured_write_bytes="$(size_to_bytes "$FIO_MEASURE_WRITE_BYTES")"
        (( configured_write_bytes % 4096 == 0 )) || die "FIO_MEASURE_WRITE_BYTES must be 4K aligned"
        write_pages=$((configured_write_bytes / 4096))
    fi

    if [[ -n "$FIO_MEASURE_READ_BYTES" && -n "$FIO_MEASURE_WRITE_BYTES" ]]; then
        :
    elif [[ -n "$FIO_MEASURE_WRITE_BYTES" ]]; then
        read_pages="$(awk -v wp="$write_pages" -v r="$read_part" -v w="$write_part" '
            BEGIN {
                if (r <= 0 || w <= 0 || wp <= 0) exit 1
                pages = int((wp * r / w) + 0.5)
                if (pages < 1) pages = 1
                print pages
            }
        ')" || die "invalid ratio '$ratio'"
    elif [[ -n "$FIO_MEASURE_READ_BYTES" ]]; then
        write_pages="$(awk -v rp="$read_pages" -v r="$read_part" -v w="$write_part" '
            BEGIN {
                if (r <= 0 || w <= 0 || rp <= 0) exit 1
                pages = int((rp * w / r) + 0.5)
                if (pages < 1) pages = 1
                print pages
            }
        ')" || die "invalid ratio '$ratio'"
    else
        total_bytes="$(size_to_bytes "$FIO_MEASURE_TOTAL_BYTES")"
        (( total_bytes % 4096 == 0 )) || die "FIO_MEASURE_TOTAL_BYTES must be 4K aligned"
        total_pages=$((total_bytes / 4096))
        read_pages="$(awk -v total="$total_pages" -v r="$read_part" -v w="$write_part" '
            BEGIN {
                if (r <= 0 || w <= 0) exit 1
                pages = int((total * r / (r + w)) + 0.5)
                if (pages < 1) pages = 1
                if (pages >= total) pages = total - 1
                print pages
            }
        ')" || die "invalid ratio '$ratio'"
        write_pages=$((total_pages - read_pages))
    fi

    echo "$((read_pages * 4096)) $((write_pages * 4096))"
}

prewarm_bytes_for_chunk() {
    local chunk_bytes="$1"
    local spec="$FIO_INIT_PREWARM_BYTES_PER_CHUNK"
    local multiplier

    if is_disabled_size "$spec"; then
        echo 0
    elif [[ "$spec" == "chunk" ]]; then
        echo "$chunk_bytes"
    elif [[ "$spec" =~ ^([0-9]+)[xX]$ ]]; then
        multiplier="${BASH_REMATCH[1]}"
        echo $((chunk_bytes * multiplier))
    elif [[ "$spec" =~ ^([0-9]+)[*]chunk$ ]]; then
        multiplier="${BASH_REMATCH[1]}"
        echo $((chunk_bytes * multiplier))
    else
        size_to_bytes "$spec"
    fi
}

init_plan_summary() {
    local region_bytes
    local init_bytes
    local chunk_size_bytes
    local chunk_count
    local offset_bytes=0
    local init_read_bytes=0

    region_bytes="$(size_to_bytes "$FIO_REGION_SIZE")"
    if is_disabled_size "$FIO_INIT_WRITE_BYTES"; then
        init_bytes=0
    else
        init_bytes="$(size_to_bytes "$FIO_INIT_WRITE_BYTES")"
    fi
    (( init_bytes <= region_bytes )) || die "FIO_INIT_WRITE_BYTES must not exceed FIO_REGION_SIZE"
    (( init_bytes % 4096 == 0 )) || die "FIO_INIT_WRITE_BYTES must be 4K aligned"

    if (( init_bytes == 0 )); then
        echo "0 0 0"
        return 0
    fi

    if [[ "$FIO_INIT_CHUNK_COUNT" =~ ^[0-9]+$ ]] && (( FIO_INIT_CHUNK_COUNT > 0 )); then
        chunk_count="$FIO_INIT_CHUNK_COUNT"
    else
        chunk_size_bytes="$(size_to_bytes "$FIO_INIT_CHUNK_SIZE")"
        (( chunk_size_bytes > 0 )) || die "FIO_INIT_CHUNK_SIZE must be positive"
        (( chunk_size_bytes % 4096 == 0 )) || die "FIO_INIT_CHUNK_SIZE must be 4K aligned"
        chunk_count=$(((init_bytes + chunk_size_bytes - 1) / chunk_size_bytes))
    fi

    if [[ "$FIO_INIT_CHUNK_COUNT" =~ ^[0-9]+$ ]] && (( FIO_INIT_CHUNK_COUNT > 0 )); then
        local base_pages=$((init_bytes / 4096 / chunk_count))
        local extra_pages=$((init_bytes / 4096 % chunk_count))
        for ((i = 0; i < chunk_count; i++)); do
            local pages=$base_pages
            if (( i < extra_pages )); then
                pages=$((pages + 1))
            fi
            local this_chunk_bytes=$((pages * 4096))
            init_read_bytes=$((init_read_bytes + $(prewarm_bytes_for_chunk "$this_chunk_bytes")))
        done
    else
        for ((i = 0; i < chunk_count; i++)); do
            local remaining=$((init_bytes - offset_bytes))
            local this_chunk_bytes="$chunk_size_bytes"
            if (( remaining < this_chunk_bytes )); then
                this_chunk_bytes="$remaining"
            fi
            init_read_bytes=$((init_read_bytes + $(prewarm_bytes_for_chunk "$this_chunk_bytes")))
            offset_bytes=$((offset_bytes + this_chunk_bytes))
        done
    fi

    echo "$chunk_count $init_bytes $init_read_bytes"
}

dist_to_fio() {
    local dist="$1"

    case "$dist" in
        zipf)
            echo "zipf:${FIO_ZIPF_ALPHA}"
            ;;
        zipf:*)
            echo "$dist"
            ;;
        normal)
            if [[ -n "$FIO_NORMAL_DEVIATION" ]]; then
                echo "normal:${FIO_NORMAL_DEVIATION}"
            else
                echo "normal"
            fi
            ;;
        normal:*)
            echo "$dist"
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

aggregate_numeric_stats_to_file() {
    local path="$1"
    local out_file="$2"
    local files=()

    mapfile -t files < <(debugfs_sibling_files "$path")
    [[ ${#files[@]} -gt 0 ]] || return 1

    sudo cat "${files[@]}" 2>/dev/null | awk '
        NF == 2 && $2 ~ /^[0-9]+$/ {
            if (!seen[$1]++) order[++n] = $1
            sum[$1] += $2
        }
        END {
            for (i = 1; i <= n; i++) print order[i], sum[order[i]]
        }
    ' >"$out_file"
}

capture_numeric_stats_aggregate() {
    local path="$1"
    local name="$2"
    local tag="$3"
    local out_dir="$4"
    local out_file="${FIO_OUTPUT_DIR%/}/${name}_aggregate_${tag}.txt"

    if aggregate_numeric_stats_to_file "$path" "$out_file"; then
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

write_run_manifest() {
    local variant="$1"
    local tag="$2"
    local out_dir="$3"
    local status="$4"
    local fio_job="$5"
    local access_dist="$6"
    local ratio="$7"
    local measure_read_bytes="$8"
    local measure_write_bytes="$9"
	local repeat_index="${10}"
    local stats_file="${FIO_OUTPUT_DIR%/}/fio_test_phase_stats_${tag}.txt"
    local manifest="${out_dir}/${tag}_manifest.json"
    local args=(
        capture
        --output "$manifest"
        --workload fio-latency-mixed
        --variant "$variant"
        --module "${NVMEV_DIR}/nvmev_${variant}.ko"
        --model nonpreemptive_submit_gate
        --status "$status"
        --artifact "$fio_job"
        --param "access_dist=${access_dist}"
        --param "ratio=${ratio}"
        --param "region_size=${FIO_REGION_SIZE}"
        --param "measure_read_bytes=${measure_read_bytes}"
        --param "measure_write_bytes=${measure_write_bytes}"
        --param "read_jobs=${FIO_MEASURE_READ_JOBS}"
        --param "write_jobs=${FIO_MEASURE_WRITE_JOBS}"
        --param "read_iodepth=${FIO_MEASURE_READ_IODEPTH}"
        --param "write_iodepth=${FIO_MEASURE_WRITE_IODEPTH}"
        --param "zipf_alpha=${FIO_ZIPF_ALPHA}"
        --param "normal_deviation=${FIO_NORMAL_DEVIATION}"
        --param "drain_after_measure=${FIO_DRAIN_AFTER_MEASURE}"
		--param "gc_nand_timing=${FIO_GC_NAND_TIMING}"
		--param "repeat_index=${repeat_index}"
		--param "randseed=$((FIO_RANDSEED_BASE + repeat_index))"
    )

    if [[ -f "$stats_file" ]]; then
        args+=(--stats "$stats_file" --artifact "$stats_file")
    fi
    if [[ "$status" == "completed" && "$FIO_STRICT_COMPILE_FLAGS" == "1" ]]; then
        args+=(--strict-contract)
    fi
    python3 "$SCRIPT_DIR/research_run_manifest.py" "${args[@]}"
}

cleanup_case_outputs() {
    local tag="$1"
    local out_dir="$2"
    local log_prefix="$3"
    local root="${FIO_OUTPUT_DIR%/}"
    local suffix

    rm -f "${out_dir}/${tag}.json" "${out_dir}/${tag}.fio"
    rm -f "${log_prefix}"_*.log
    rm -f "${root}/fio_test_phase_stats_${tag}.txt" \
          "${root}/fio_test_phase_stats_aggregate_${tag}.txt" \
          "${root}/fio_superblock_stats_${tag}.txt" \
          "${root}/fio_die_stats_${tag}.txt" \
          "${root}/fio_bg_nand_stats_${tag}.txt"
    rm -f "${out_dir}/fio_test_phase_stats_${tag}.txt" \
          "${out_dir}/fio_test_phase_stats_aggregate_${tag}.txt" \
          "${out_dir}/fio_superblock_stats_${tag}.txt" \
          "${out_dir}/fio_die_stats_${tag}.txt" \
          "${out_dir}/fio_bg_nand_stats_${tag}.txt"
    for suffix in post_drain; do
        rm -f "${root}/fio_test_phase_stats_${tag}_${suffix}.txt" \
              "${root}/fio_test_phase_stats_aggregate_${tag}_${suffix}.txt" \
              "${root}/fio_superblock_stats_${tag}_${suffix}.txt" \
              "${root}/fio_die_stats_${tag}_${suffix}.txt" \
              "${root}/fio_bg_nand_stats_${tag}_${suffix}.txt"
        rm -f "${out_dir}/fio_test_phase_stats_${tag}_${suffix}.txt" \
              "${out_dir}/fio_test_phase_stats_aggregate_${tag}_${suffix}.txt" \
              "${out_dir}/fio_superblock_stats_${tag}_${suffix}.txt" \
              "${out_dir}/fio_die_stats_${tag}_${suffix}.txt" \
              "${out_dir}/fio_bg_nand_stats_${tag}_${suffix}.txt"
    done
    rm -f "${root}/fio_test_phase_stats_aggregate_${tag}_foreground_post_drain.txt" \
          "${out_dir}/fio_test_phase_stats_aggregate_${tag}_foreground_post_drain.txt" \
          "${root}/fio_post_drain_${tag}.txt" \
          "${out_dir}/fio_post_drain_${tag}.txt"
}

write_foreground_post_drain_aggregate() {
    local tag="$1"
    local out_dir="$2"
    local root="${FIO_OUTPUT_DIR%/}"
    local fg="${out_dir}/fio_test_phase_stats_aggregate_${tag}.txt"
    local post="${out_dir}/fio_test_phase_stats_aggregate_${tag}_post_drain.txt"
    local out_file="${root}/fio_test_phase_stats_aggregate_${tag}_foreground_post_drain.txt"

    [[ -f "$fg" && -f "$post" ]] || return 0
    awk '
        BEGIN { print "key foreground post_drain delta" }
        FNR == NR && NF == 2 && $2 ~ /^-?[0-9]+$/ {
            if (!seen[$1]++) order[++n] = $1
            fg[$1] = $2
            next
        }
        NF == 2 && $2 ~ /^-?[0-9]+$/ {
            if (!seen[$1]++) order[++n] = $1
            post[$1] = $2
            next
        }
        END {
            for (i = 1; i <= n; i++) {
                key = order[i]
                f = (key in fg) ? fg[key] : ""
                p = (key in post) ? post[key] : ""
                d = ((key in fg) && (key in post)) ? post[key] - fg[key] : ""
                print key, f, p, d
            }
        }
    ' "$fg" "$post" >"$out_file"
    cp "$out_file" "$out_dir/" 2>/dev/null || true
}

write_post_drain_bundle() {
    local tag="$1"
    local out_dir="$2"
    local root="${FIO_OUTPUT_DIR%/}"
    local out_file="${root}/fio_post_drain_${tag}.txt"
    local file
    local files=(
        "${out_dir}/fio_test_phase_stats_${tag}_post_drain.txt"
        "${out_dir}/fio_test_phase_stats_aggregate_${tag}_post_drain.txt"
        "${out_dir}/fio_superblock_stats_${tag}_post_drain.txt"
        "${out_dir}/fio_die_stats_${tag}_post_drain.txt"
        "${out_dir}/fio_bg_nand_stats_${tag}_post_drain.txt"
        "${out_dir}/fio_test_phase_stats_aggregate_${tag}_foreground_post_drain.txt"
    )

    : >"$out_file"
    for file in "${files[@]}"; do
        [[ -f "$file" ]] || continue
        {
            echo "===== $(basename "$file") ====="
            cat "$file"
            echo ""
        } >>"$out_file"
    done
    cp "$out_file" "$out_dir/" 2>/dev/null || true
}

numeric_stat_value() {
    local key="$1"
    local file="$2"

    awk -v key="$key" '$1 == key { value = $2 } END { print value + 0 }' "$file"
}

numeric_stats_signature() {
    local file="$1"

    awk -v keys="$FIO_DRAIN_STABLE_KEYS" '
        BEGIN { n = split(keys, key_order, " ") }
        NF == 2 && $2 ~ /^[0-9]+$/ { value[$1] = $2 }
        END {
            for (i = 1; i <= n; i++) {
                key = key_order[i]
                printf "%s=%s;", key, (key in value ? value[key] : 0)
            }
        }
    ' "$file"
}

wait_for_test_phase_drain() {
    local tag="$1"
    local tmp_stats
    local elapsed=0
    local stable=0
    local last_sig=""
    local sig
    local active_reads
    local active_overwrites
    local active_bg_ops
    local bg_repromote
    local bg_qlc_rebalance
    local internal_write

    [[ "$FIO_DRAIN_AFTER_MEASURE" == "1" ]] || return 0

    for value in "$FIO_DRAIN_MIN_SEC" "$FIO_DRAIN_TIMEOUT_SEC" \
        "$FIO_DRAIN_POLL_SEC" "$FIO_DRAIN_STABLE_POLLS"; do
        [[ "$value" =~ ^[0-9]+$ ]] || die "drain controls must be integer seconds/counts"
    done
    (( FIO_DRAIN_POLL_SEC > 0 )) || die "FIO_DRAIN_POLL_SEC must be positive"
    (( FIO_DRAIN_STABLE_POLLS > 0 )) || die "FIO_DRAIN_STABLE_POLLS must be positive"

    tmp_stats="$(mktemp "${TMPDIR:-/tmp}/fio_latency_mixed_drain_${tag}.XXXXXX.stats")"
    echo "[fio-drain] enabled: min=${FIO_DRAIN_MIN_SEC}s timeout=${FIO_DRAIN_TIMEOUT_SEC}s poll=${FIO_DRAIN_POLL_SEC}s stable_polls=${FIO_DRAIN_STABLE_POLLS}"

    while (( elapsed <= FIO_DRAIN_TIMEOUT_SEC )); do
        if ! aggregate_numeric_stats_to_file "$FIO_TEST_PHASE_STATS_PATH" "$tmp_stats"; then
            echo "[fio-drain] unable to read $FIO_TEST_PHASE_STATS_PATH; skipping drain wait" >&2
            rm -f "$tmp_stats"
            return 0
        fi

        active_reads="$(numeric_stat_value active_reads "$tmp_stats")"
        active_overwrites="$(numeric_stat_value active_overwrites "$tmp_stats")"
        active_bg_ops="$(numeric_stat_value active_bg_ops "$tmp_stats")"
        bg_repromote="$(numeric_stat_value bg_repromote_ops "$tmp_stats")"
        bg_qlc_rebalance="$(numeric_stat_value bg_qlc_rebalance_ops "$tmp_stats")"
        internal_write="$(numeric_stat_value internal_write_pages_est "$tmp_stats")"
        sig="$(numeric_stats_signature "$tmp_stats")"

        echo "[fio-drain] t=${elapsed}s active_reads=$active_reads active_writes=$active_overwrites active_bg=$active_bg_ops bg_repromote=$bg_repromote bg_qlc_rebalance=$bg_qlc_rebalance internal_write_pages_est=$internal_write stable=$stable"

        if (( elapsed >= FIO_DRAIN_MIN_SEC )) && \
            (( active_reads == 0 && active_overwrites == 0 && active_bg_ops == 0 )) && \
            [[ "$sig" == "$last_sig" ]]; then
            stable=$((stable + 1))
        else
            stable=0
        fi

        if (( stable >= FIO_DRAIN_STABLE_POLLS )); then
            echo "[fio-drain] stable after ${elapsed}s"
            rm -f "$tmp_stats"
            return 0
        fi

        last_sig="$sig"
        sleep "$FIO_DRAIN_POLL_SEC"
        elapsed=$((elapsed + FIO_DRAIN_POLL_SEC))
    done

    echo "[fio-drain] timed out after ${FIO_DRAIN_TIMEOUT_SEC}s; capturing post-drain stats anyway" >&2
    rm -f "$tmp_stats"
    return 0
}

expected_compile_flags_for_variant() {
    local variant="$1"

    case "$variant" in
        die_latency*_qlc_all_norp_sb)
            cat <<'EOF'
compile_read_repromotion_enabled 0
compile_die_batched_repromotion_enabled 0
compile_qlc_hotcold_enabled 1
compile_qlc_rebalance_enabled 0
compile_test_phase_repromotion_enabled 0
compile_test_phase_qlc_rebalance_enabled 0
EOF
            ;;
        die_latency1_norp_sb)
            cat <<'EOF'
compile_read_repromotion_enabled 0
compile_die_batched_repromotion_enabled 0
compile_qlc_hotcold_enabled 0
compile_qlc_rebalance_enabled 0
compile_test_phase_repromotion_enabled 0
compile_test_phase_qlc_rebalance_enabled 0
EOF
            ;;
        die_latency[23]_norp_sb)
            cat <<'EOF'
compile_read_repromotion_enabled 0
compile_die_batched_repromotion_enabled 0
compile_qlc_hotcold_enabled 0
compile_qlc_rebalance_enabled 0
compile_test_phase_repromotion_enabled 0
compile_test_phase_qlc_rebalance_enabled 0
EOF
            ;;
        *)
            return 1
            ;;
    esac
}

check_compile_flags() {
    local variant="$1"
    local tag="$2"
    local stats_file="${FIO_OUTPUT_DIR%/}/fio_test_phase_stats_${tag}.txt"
    local expected
    local key
    local expected_value
    local actual_value
    local errors=0

    expected="$(expected_compile_flags_for_variant "$variant" || true)"
    [[ -n "$expected" ]] || return 0
    [[ -f "$stats_file" ]] || return 0

    while read -r key expected_value; do
        [[ -n "$key" ]] || continue
        actual_value="$(awk -v key="$key" '$1 == key { print $2; found = 1; exit } END { if (!found) print "MISSING" }' "$stats_file")"
        if [[ "$actual_value" != "$expected_value" ]]; then
            echo "[fio-compile-flags] $variant $key actual=$actual_value expected=$expected_value" >&2
            errors=$((errors + 1))
        fi
    done <<<"$expected"

    if (( errors > 0 )); then
        if [[ "$FIO_STRICT_COMPILE_FLAGS" == "1" ]]; then
            return 1
        fi
        echo "[fio-compile-flags] WARN $variant has unexpected compile flags; set FIO_STRICT_COMPILE_FLAGS=1 to fail the run" >&2
    else
        echo "[fio-compile-flags] PASS $variant"
    fi
}

validate_fio_json() {
    local out_json="$1"
    local expected_init_write="$2"
    local expected_init_prewarm="$3"
    local expected_measure_read="$4"
    local expected_measure_write="$5"

    if ! command -v python3 >/dev/null 2>&1; then
        echo "[fio-validate] python3 is required to validate fio JSON output" >&2
        return 1
    fi

    python3 "$SCRIPT_DIR/fio_validate_latency_mixed_json.py" "$out_json" \
        --expected-init-write "$expected_init_write" \
        --expected-init-prewarm "$expected_init_prewarm" \
        --expected-measure-read "$expected_measure_read" \
        --expected-measure-write "$expected_measure_write"
}

validate_generated_fio_job() {
    local fio_job="$1"
    local expected_measure_write_region_size="$2"

    python3 - "$fio_job" "$expected_measure_write_region_size" <<'PY'
import sys
from pathlib import Path

units = {
    "": 1,
    "K": 1024,
    "M": 1024**2,
    "G": 1024**3,
    "T": 1024**4,
}


def size_to_bytes(raw):
    value = str(raw).strip()
    if not value:
        raise ValueError("empty size")
    unit = value[-1:].upper()
    if unit in units and unit:
        number = value[:-1]
    else:
        unit = ""
        number = value
    if not number.isdigit():
        raise ValueError(f"invalid size {raw!r}")
    return int(number) * units[unit]


path = Path(sys.argv[1])
expected_write_region = int(sys.argv[2])
sections = {}
current = None

for raw in path.read_text(errors="replace").splitlines():
    line = raw.strip()
    if not line or line.startswith(";"):
        continue
    if line.startswith("[") and line.endswith("]"):
        current = line[1:-1]
        sections[current] = {}
        continue
    if current and "=" in line:
        key, value = line.split("=", 1)
        sections[current][key.strip()] = value.strip()

errors = []
for name, opts in sections.items():
    if "offset" not in opts or "size" not in opts:
        continue
    try:
        offset = size_to_bytes(opts["offset"])
        size = size_to_bytes(opts["size"])
    except ValueError as exc:
        errors.append(f"{name}: {exc}")
        continue
    if size <= offset:
        errors.append(f"{name}: size={size} must be greater than offset={offset}")

if "measure_writes" not in sections:
    errors.append("measure_writes section is missing")
else:
    opts = sections["measure_writes"]
    try:
        span = size_to_bytes(opts["size"]) - size_to_bytes(opts["offset"])
    except KeyError as exc:
        errors.append(f"measure_writes: missing {exc.args[0]}")
    except ValueError as exc:
        errors.append(f"measure_writes: {exc}")
    else:
        if span != expected_write_region:
            errors.append(
                "measure_writes: size-offset span "
                f"{span} != expected write region {expected_write_region}"
            )

if errors:
    print("[fio-job-validate] FAIL", file=sys.stderr)
    for error in errors:
        print(f"[fio-job-validate]   {error}", file=sys.stderr)
    sys.exit(1)

print(f"[fio-job-validate] PASS {path}")
PY
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

emit_guard_toggle_job() {
    local name="$1"
    local value="$2"
    local offset_bytes="$3"
    local test_phase_hook_path="$4"

    [[ -n "$test_phase_hook_path" ]] || return 0

    echo "[$name]"
    echo "description=Set FTL test_phase=$value for guarded init prewarm."
    echo "rw=read"
    echo "offset=$offset_bytes"
    echo "size=$(fio_size_end "$offset_bytes" 4096)"
    echo "io_size=4k"
    echo "iodepth=1"
    echo "numjobs=1"
    echo "exec_prerun=echo $value > $test_phase_hook_path"
    echo "stonewall"
    echo ""
}

emit_heat_epoch_advance_job() {
    local name="$1"
    local offset_bytes="$2"
    local heat_epoch_path="$3"

    [[ -n "$heat_epoch_path" ]] || return 0
    [[ "$FIO_INIT_ADVANCE_HEAT_EPOCH_EACH_CHUNK" == "1" ]] || return 0

    echo "[$name]"
    echo "description=Advance FTL heat_epoch after guarded init prewarm."
    echo "rw=read"
    echo "offset=$offset_bytes"
    echo "size=$(fio_size_end "$offset_bytes" 4096)"
    echo "io_size=4k"
    echo "iodepth=1"
    echo "numjobs=1"
    echo "exec_prerun=echo 1 > $heat_epoch_path"
    echo "stonewall"
    echo ""
}

emit_init_chunk() {
    local index="$1"
    local offset_bytes="$2"
    local chunk_bytes="$3"
    local fio_dist="$4"
    local test_phase_hook_path="$5"
    local heat_epoch_path="$6"
    local idx
    local suffix
    local prewarm_io_size

    printf -v idx "%03d" "$index"
    suffix="_$idx"
    if [[ "$FIO_INIT_CHUNK_COUNT" == "1" ]]; then
        suffix=""
    fi

    echo "[init_write${suffix}]"
    echo "description=Init: write initialized region before prewarming it."
    echo "rw=write"
    echo "offset=$offset_bytes"
    echo "size=$(fio_size_end "$offset_bytes" "$chunk_bytes")"
    echo "io_size=$chunk_bytes"
    echo "iodepth=32"
    echo "numjobs=1"
    echo "stonewall"
    echo ""

    prewarm_io_size="$(prewarm_bytes_for_chunk "$chunk_bytes")"
    if (( prewarm_io_size == 0 )); then
        return 0
    fi

    if [[ "$FIO_INIT_PREWARM_GUARD" == "1" ]]; then
        emit_guard_toggle_job "init_guard_on${suffix}" 1 "$offset_bytes" "$test_phase_hook_path"
    fi

    echo "[init_prewarm${suffix}]"
    echo "description=Init: distribution-shaped read prewarm over the just-written region."
    echo "rw=randread"
    echo "random_distribution=$fio_dist"
    echo "offset=$offset_bytes"
    echo "size=$(fio_size_end "$offset_bytes" "$chunk_bytes")"
    echo "io_size=$prewarm_io_size"
    echo "iodepth=$FIO_INIT_PREWARM_IODEPTH"
    echo "numjobs=$FIO_INIT_PREWARM_JOBS"
    echo "stonewall"
    echo ""

    if [[ "$FIO_INIT_PREWARM_GUARD" == "1" ]]; then
        emit_guard_toggle_job "init_guard_off${suffix}" 0 "$offset_bytes" "$test_phase_hook_path"
    fi
    emit_heat_epoch_advance_job "init_heat_epoch_advance${suffix}" "$offset_bytes" "$heat_epoch_path"
}

render_fio_job() {
    local out_job="$1"
    local fio_dist="$2"
    local mixread="$3"
    local log_prefix="$4"
    local test_phase_hook_path="$5"
    local measure_read_bytes="$6"
    local measure_write_bytes="$7"
    local heat_epoch_path="${8:-}"
    local region_bytes
    local init_bytes
    local chunk_size_bytes
    local chunk_count
    local base_pages
    local extra_pages
    local offset_bytes=0
    local prewarm_bytes
    local page_bytes=4096
    local init_plan
    local init_plan_chunks
    local init_plan_write_bytes
    local init_plan_read_bytes
    local write_region_size
    local write_region_size_bytes
    local write_offset_bytes
    local write_size_end

    region_bytes="$(size_to_bytes "$FIO_REGION_SIZE")"
    if is_disabled_size "$FIO_INIT_WRITE_BYTES"; then
        init_bytes=0
    else
        init_bytes="$(size_to_bytes "$FIO_INIT_WRITE_BYTES")"
    fi
    (( init_bytes <= region_bytes )) || die "FIO_INIT_WRITE_BYTES must not exceed FIO_REGION_SIZE"
    (( init_bytes % page_bytes == 0 )) || die "FIO_INIT_WRITE_BYTES must be 4K aligned"

    init_plan="$(init_plan_summary)"
    read -r init_plan_chunks init_plan_write_bytes init_plan_read_bytes <<<"$init_plan"
    if [[ "$FIO_MEASURE_WRITE_REGION_SIZE" == "io_size" ]]; then
        write_region_size_bytes="$measure_write_bytes"
    else
        write_region_size_bytes="$(size_to_bytes "$FIO_MEASURE_WRITE_REGION_SIZE")"
    fi
    (( write_region_size_bytes % page_bytes == 0 )) || die "FIO_MEASURE_WRITE_REGION_SIZE must be 4K aligned"
    write_region_size="$write_region_size_bytes"
    write_offset_bytes="$(size_to_bytes "$FIO_MEASURE_WRITE_OFFSET")"
    (( write_offset_bytes % page_bytes == 0 )) || die "FIO_MEASURE_WRITE_OFFSET must be 4K aligned"
    write_size_end="$(fio_size_end "$write_offset_bytes" "$write_region_size_bytes")"

    {
        echo "; Generated by fio_fragment_die_latency_mixed.sh"
        echo "; variant/access/ratio are encoded in the output path."
        echo "; init writes are chunked; each chunk is prewarmed before the next chunk."
        echo "; init_chunk_count=$init_plan_chunks"
        echo "; init_write_bytes=$init_plan_write_bytes"
        echo "; init_prewarm_read_bytes=$init_plan_read_bytes"
        echo "; init_advance_heat_epoch_each_chunk=$FIO_INIT_ADVANCE_HEAT_EPOCH_EACH_CHUNK"
        echo "; measure_read_bytes=$measure_read_bytes"
        echo "; measure_write_bytes=$measure_write_bytes"
        echo "; measure_read_offset=0"
        echo "; measure_write_offset=$FIO_MEASURE_WRITE_OFFSET"
        echo "; measure_write_region_size=$write_region_size"
        echo "; measure_write_fio_size_end=$write_size_end"
        echo "; fio size is emitted as offset+range for non-zero-offset jobs."
        echo ""
        echo "[global]"
        echo "ioengine=libaio"
        echo "direct=1"
        echo "thread=1"
        echo "group_reporting=0"
        echo "time_based=0"
        echo "norandommap=1"
	    echo "randrepeat=1"
	    echo "randseed=$((FIO_RANDSEED_BASE + repeat_index))"
        echo "invalidate=1"
        echo "bs=4k"
        echo "iodepth=32"
        echo "filename=$DATA_DEV"
        echo ""

        if (( init_bytes > 0 )); then
            if [[ "$FIO_INIT_CHUNK_COUNT" =~ ^[0-9]+$ ]] && (( FIO_INIT_CHUNK_COUNT > 0 )); then
                chunk_count="$FIO_INIT_CHUNK_COUNT"
                (( init_bytes / page_bytes >= chunk_count )) || die "FIO_INIT_CHUNK_COUNT is larger than the 4K page count in init bytes"
                base_pages=$((init_bytes / page_bytes / chunk_count))
                extra_pages=$((init_bytes / page_bytes % chunk_count))
                for ((i = 0; i < chunk_count; i++)); do
                    local pages=$base_pages
                    if (( i < extra_pages )); then
                        pages=$((pages + 1))
                    fi
                    local this_chunk_bytes=$((pages * page_bytes))
                    emit_init_chunk "$i" "$offset_bytes" "$this_chunk_bytes" "$fio_dist" "$test_phase_hook_path" "$heat_epoch_path"
                    offset_bytes=$((offset_bytes + this_chunk_bytes))
                done
            else
                chunk_size_bytes="$(size_to_bytes "$FIO_INIT_CHUNK_SIZE")"
                (( chunk_size_bytes > 0 )) || die "FIO_INIT_CHUNK_SIZE must be positive"
                (( chunk_size_bytes % page_bytes == 0 )) || die "FIO_INIT_CHUNK_SIZE must be 4K aligned"
                chunk_count=$(((init_bytes + chunk_size_bytes - 1) / chunk_size_bytes))
                for ((i = 0; i < chunk_count; i++)); do
                    local remaining=$((init_bytes - offset_bytes))
                    local this_chunk_bytes="$chunk_size_bytes"
                    if (( remaining < this_chunk_bytes )); then
                        this_chunk_bytes="$remaining"
                    fi
                    emit_init_chunk "$i" "$offset_bytes" "$this_chunk_bytes" "$fio_dist" "$test_phase_hook_path" "$heat_epoch_path"
                    offset_bytes=$((offset_bytes + this_chunk_bytes))
                done
            fi
        fi

        if ! is_disabled_size "$FIO_PREWARM_SEQ_BYTES"; then
            echo "[prewarm_seq_read]"
            echo "description=Optional post-init sequential read over the initialized region."
            echo "rw=read"
            echo "offset=0"
            echo "size=$FIO_REGION_SIZE"
            echo "io_size=$FIO_PREWARM_SEQ_BYTES"
            echo "iodepth=32"
            echo "numjobs=1"
            echo "stonewall"
            echo ""
        fi

        if ! is_disabled_size "$FIO_PREWARM_RANDOM_BYTES"; then
            echo "[prewarm_dist_read]"
            echo "description=Optional post-init distribution-shaped random read warmup."
            echo "rw=randread"
            echo "random_distribution=$fio_dist"
            echo "offset=0"
            echo "size=$FIO_REGION_SIZE"
            echo "io_size=$FIO_PREWARM_RANDOM_BYTES"
            echo "iodepth=$FIO_PREWARM_IODEPTH"
            echo "numjobs=$FIO_PREWARM_JOBS"
            echo "stonewall"
            echo ""
        fi

        echo "[enter_test_phase]"
        echo "description=Enable FTL test_phase immediately before measurement."
        echo "rw=read"
        echo "offset=0"
        echo "size=4k"
        echo "io_size=4k"
        echo "iodepth=1"
        echo "numjobs=1"
        if [[ -n "$test_phase_hook_path" ]]; then
            echo "exec_prerun=echo 1 > $test_phase_hook_path"
        fi
        echo "stonewall"
        echo ""

        echo "[measure_reads]"
        echo "description=Measured phase reads: distribution-shaped reads from init-only region."
        echo "rw=randread"
        echo "random_distribution=$fio_dist"
        echo "offset=0"
        echo "size=$FIO_REGION_SIZE"
        echo "io_size=$measure_read_bytes"
        echo "iodepth=$FIO_MEASURE_READ_IODEPTH"
        echo "numjobs=$FIO_MEASURE_READ_JOBS"
        echo "write_lat_log=${log_prefix}_read"
        echo "write_bw_log=${log_prefix}_read"
        echo "write_iops_log=${log_prefix}_read"
        echo "log_avg_msec=1000"
        if [[ -n "$FIO_MEASURE_RATE" ]]; then
            echo "rate=$FIO_MEASURE_RATE"
        fi
        if [[ -n "$FIO_MEASURE_RATE_IOPS" ]]; then
            echo "rate_iops=$FIO_MEASURE_RATE_IOPS"
        fi
        echo ""

        echo "[measure_writes]"
        echo "description=Measured phase writes: random writes to a separate non-read region."
        echo "rw=randwrite"
        echo "random_distribution=$fio_dist"
        echo "offset=$write_offset_bytes"
        echo "size=$write_size_end"
        echo "io_size=$measure_write_bytes"
        echo "iodepth=$FIO_MEASURE_WRITE_IODEPTH"
        echo "numjobs=$FIO_MEASURE_WRITE_JOBS"
        echo "write_lat_log=${log_prefix}_write"
        echo "write_bw_log=${log_prefix}_write"
        echo "write_iops_log=${log_prefix}_write"
        echo "log_avg_msec=1000"
    } >"$out_job"
}

run_fio_one_case() {
    local variant="$1"
    local access_dist="$2"
    local ratio="$3"
	local repeat_index="$4"
    local mixread
    local fio_dist
    local measure_bytes
    local measure_read_bytes
    local measure_write_bytes
    local init_plan
    local init_plan_chunks
    local init_plan_write_bytes
    local init_plan_read_bytes
    local tag
    local out_dir
    local out_json
    local fio_job_tmp
    local fio_job_saved
    local log_prefix
    local test_phase_hook_path=""
    local heat_epoch_hook_path=""
    local rc=0
    local measure_write_offset_bytes
    local measure_write_region_size_bytes

    mixread="$(ratio_to_mixread "$ratio")"
    fio_dist="$(dist_to_fio "$access_dist")"
    measure_bytes="$(ratio_to_measure_bytes "$ratio")"
    read -r measure_read_bytes measure_write_bytes <<<"$measure_bytes"
    measure_write_offset_bytes="$(size_to_bytes "$FIO_MEASURE_WRITE_OFFSET")"
    if [[ "$FIO_MEASURE_WRITE_REGION_SIZE" == "io_size" ]]; then
        measure_write_region_size_bytes="$measure_write_bytes"
    else
        measure_write_region_size_bytes="$(size_to_bytes "$FIO_MEASURE_WRITE_REGION_SIZE")"
    fi
    init_plan="$(init_plan_summary)"
    read -r init_plan_chunks init_plan_write_bytes init_plan_read_bytes <<<"$init_plan"
	 tag="fio_mixed_${variant}_$(dist_tag "$access_dist")_rw$(ratio_tag "$ratio")_bg${FIO_GC_NAND_TIMING}_r${repeat_index}"
	 out_dir="${FIO_OUTPUT_DIR%/}/${variant}/$(dist_tag "$access_dist")/rw$(ratio_tag "$ratio")/bg${FIO_GC_NAND_TIMING}/r${repeat_index}"
    out_json="${out_dir}/${tag}.json"
    fio_job_tmp="$(mktemp "${TMPDIR:-/tmp}/fio_latency_v2_mixed_${variant}.XXXXXX.fio")"
    fio_job_saved="${out_dir}/${tag}.fio"
    log_prefix="${out_dir}/${tag}"

    mkdir -p "$out_dir"
    cleanup_case_outputs "$tag" "$out_dir" "$log_prefix"

    echo ""
    echo "================================================================"
    echo "  [FIO-LATENCY-MIXED] variant=$variant dist=$access_dist($fio_dist) ratio=$ratio"
    echo "  region=$FIO_REGION_SIZE init_write=$FIO_INIT_WRITE_BYTES init_chunk_size=$FIO_INIT_CHUNK_SIZE init_chunk_count=$FIO_INIT_CHUNK_COUNT init_prewarm_per_chunk=$FIO_INIT_PREWARM_BYTES_PER_CHUNK init_prewarm_guard=$FIO_INIT_PREWARM_GUARD"
    echo "  init_plan_chunks=$init_plan_chunks init_write_bytes=$init_plan_write_bytes init_prewarm_read_bytes=$init_plan_read_bytes"
    echo "  init_advance_heat_epoch_each_chunk=$FIO_INIT_ADVANCE_HEAT_EPOCH_EACH_CHUNK heat_epoch_path=$FIO_HEAT_EPOCH_PATH"
    echo "  post_prewarm_seq=$FIO_PREWARM_SEQ_BYTES post_prewarm_random=$FIO_PREWARM_RANDOM_BYTES measure_total_fallback=$FIO_MEASURE_TOTAL_BYTES"
    echo "  measure_read_bytes=$measure_read_bytes measure_write_bytes=$measure_write_bytes read_range=0+$FIO_REGION_SIZE write_range=${measure_write_offset_bytes}+${measure_write_region_size_bytes}"
    echo "  measure_jobs=$FIO_MEASURE_JOBS measure_iodepth=$FIO_MEASURE_IODEPTH gc_nand_timing=$FIO_GC_NAND_TIMING recent_write_guard=$FIO_TEST_PHASE_RECENT_WRITE_GUARD guard_read_reqs=$FIO_TEST_PHASE_GUARD_READ_REQS"
    echo "  drain_after_measure=$FIO_DRAIN_AFTER_MEASURE drain_min=${FIO_DRAIN_MIN_SEC}s drain_timeout=${FIO_DRAIN_TIMEOUT_SEC}s strict_compile_flags=$FIO_STRICT_COMPILE_FLAGS"
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
        if [[ -e "$FIO_HEAT_EPOCH_PATH" ]]; then
            heat_epoch_hook_path="$FIO_HEAT_EPOCH_PATH"
        else
            echo "[heat_epoch] $FIO_HEAT_EPOCH_PATH not present; generated job will run without init heat_epoch advance hook" >&2
        fi
    else
        test_phase_hook_path="$FIO_TEST_PHASE_PATH"
        heat_epoch_hook_path="$FIO_HEAT_EPOCH_PATH"
    fi

    render_fio_job "$fio_job_tmp" "$fio_dist" "$mixread" "$log_prefix" "$test_phase_hook_path" "$measure_read_bytes" "$measure_write_bytes" "$heat_epoch_hook_path"
    validate_generated_fio_job "$fio_job_tmp" "$measure_write_region_size_bytes"
    cp "$fio_job_tmp" "$fio_job_saved"

    write_run_manifest "$variant" "$tag" "$out_dir" \
        "prepared" "$fio_job_saved" "$access_dist" "$ratio" \
	    "$measure_read_bytes" "$measure_write_bytes" "$repeat_index"

    echo "=== fio jobfile: $fio_job_saved ==="
    if [[ "$FIO_DRY_RUN" == "1" ]]; then
        write_run_manifest "$variant" "$tag" "$out_dir" \
            "dry_run" "$fio_job_saved" "$access_dist" "$ratio" \
	        "$measure_read_bytes" "$measure_write_bytes" "$repeat_index"
        rm -f "$fio_job_tmp"
        return 0
    fi

    if sudo fio "$fio_job_tmp" --output="$out_json" --output-format=json; then
        if validate_fio_json "$out_json" \
            "$init_plan_write_bytes" "$init_plan_read_bytes" \
            "$measure_read_bytes" "$measure_write_bytes"; then
            rc=0
        else
            rc=$?
        fi
    else
        rc=$?
    fi

    if [[ "$FIO_DRAIN_AFTER_MEASURE" == "1" ]]; then
        echo "=== Capturing foreground stats before post-measure drain ==="
        capture_run_stats "$tag" "$out_dir"
        check_compile_flags "$variant" "$tag" || rc=$?
        wait_for_test_phase_drain "$tag"
        echo "=== Capturing post-drain stats ==="
        capture_run_stats "${tag}_post_drain" "$out_dir"
        write_foreground_post_drain_aggregate "$tag" "$out_dir"
        write_post_drain_bundle "$tag" "$out_dir"
        set_test_phase 0 || true
    else
        set_test_phase 0 || true
        capture_run_stats "$tag" "$out_dir"
        check_compile_flags "$variant" "$tag" || rc=$?
    fi
    write_run_manifest "$variant" "$tag" "$out_dir" \
        "completed" "$fio_job_saved" "$access_dist" "$ratio" \
	    "$measure_read_bytes" "$measure_write_bytes" "$repeat_index" || rc=$?
    rm -f "$fio_job_tmp"
    sudo rmmod nvmev 2>/dev/null || rmmod nvmev 2>/dev/null || true
    sleep 5
    return "$rc"
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

[[ "$FIO_REPEAT_COUNT" =~ ^[1-9][0-9]*$ ]] || die "FIO_REPEAT_COUNT must be a positive integer"
[[ "$FIO_RANDSEED_BASE" =~ ^[0-9]+$ ]] || die "FIO_RANDSEED_BASE must be a non-negative integer"

for ((repeat_index = 1; repeat_index <= FIO_REPEAT_COUNT; repeat_index++)); do
    for variant in $VARIANTS; do
        for access_dist in $FIO_ACCESS_DIST_LIST; do
            for ratio in $FIO_RW_RATIOS; do
                run_fio_one_case "$variant" "$access_dist" "$ratio" "$repeat_index"
	    done
        done
    done
done

echo ""
echo "========================================"
echo "  [FIO-LATENCY-MIXED] All tests completed."
echo "  Results in: $FIO_OUTPUT_DIR"
echo "========================================"
