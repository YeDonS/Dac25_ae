#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

source commonvariables.sh

NVMEV_DIR="${SCRIPT_DIR}/../nvmevirt_DA"
VARIANTS="${VARIANTS:-die_base_sb die_latency_sb_v1 die_latency_sb die_all_sb}"
RUN_SQLITE="${RUN_SQLITE:-1}"
RUN_FIO="${RUN_FIO:-1}"
FIO_JOB_FILE="${FIO_JOB_FILE:-fio_latency_v2_mixed.fio}"
FIO_READ_JOBS="${FIO_READ_JOBS:-}"
FIO_WRITE_JOBS="${FIO_WRITE_JOBS:-}"
FIO_READ_IODEPTH="${FIO_READ_IODEPTH:-}"
FIO_WRITE_IODEPTH="${FIO_WRITE_IODEPTH:-}"
FIO_MEASURE_READ_BYTES="${FIO_MEASURE_READ_BYTES:-}"
FIO_MEASURE_WRITE_BYTES="${FIO_MEASURE_WRITE_BYTES:-}"
FIO_READ_RATE="${FIO_READ_RATE:-}"
FIO_WRITE_RATE="${FIO_WRITE_RATE:-}"
FIO_READ_RATE_IOPS="${FIO_READ_RATE_IOPS:-}"
FIO_WRITE_RATE_IOPS="${FIO_WRITE_RATE_IOPS:-}"
FIO_TEST_PHASE_PATH="${FIO_TEST_PHASE_PATH:-/sys/kernel/debug/nvmev/ftl0/test_phase}"

mkdir -p "$RESULT_FOLDER"

drop_caches() {
    sync
    echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
}

load_variant_module() {
    local variant="$1"
    local ko_path="${NVMEV_DIR}/nvmev_${variant}.ko"

    if [[ ! -f "$ko_path" ]]; then
        echo "ERROR: $ko_path not found. Build it first or set SQLITE_REBUILD_DIE_MODULES=1." >&2
        exit 1
    fi

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

run_fio_one_variant() {
    local variant="$1"
    local ts
    local out_json
    local fio_job_tmp

    ts="$(date +%Y%m%d_%H%M%S)"
    out_json="${RESULT_FOLDER%/}/fio_latency_mixed_${variant}_${ts}.json"
    fio_job_tmp="$(mktemp "${TMPDIR:-/tmp}/fio_latency_v2_mixed_${variant}.XXXXXX.fio")"

    load_variant_module "$variant"
    lsblk
    echo 0 | sudo tee /sys/block/${DATA_NAME}/queue/read_ahead_kb >/dev/null 2>&1 || true
    drop_caches

    if [[ -w "$FIO_TEST_PHASE_PATH" ]]; then
        echo 0 | sudo tee "$FIO_TEST_PHASE_PATH" >/dev/null || true
    fi

    awk \
        -v filename="$DATA_DEV" \
        -v test_phase_path="$FIO_TEST_PHASE_PATH" \
        -v read_jobs="$FIO_READ_JOBS" \
        -v write_jobs="$FIO_WRITE_JOBS" \
        -v read_iodepth="$FIO_READ_IODEPTH" \
        -v write_iodepth="$FIO_WRITE_IODEPTH" \
        -v read_io_size="$FIO_MEASURE_READ_BYTES" \
        -v write_io_size="$FIO_MEASURE_WRITE_BYTES" \
        -v read_rate="$FIO_READ_RATE" \
        -v write_rate="$FIO_WRITE_RATE" \
        -v read_rate_iops="$FIO_READ_RATE_IOPS" \
        -v write_rate_iops="$FIO_WRITE_RATE_IOPS" '
        function emit_measure_extras() {
            if (section == "measure_reads" && !read_done) {
                print "exec_prerun=echo 1 > " test_phase_path
                if (read_rate != "") print "rate=" read_rate
                if (read_rate_iops != "") print "rate_iops=" read_rate_iops
                read_done = 1
            } else if (section == "measure_writes" && !write_done) {
                print "exec_prerun=echo 1 > " test_phase_path
                if (write_rate != "") print "rate=" write_rate
                if (write_rate_iops != "") print "rate_iops=" write_rate_iops
                write_done = 1
            }
        }
        /^\[/ {
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
        section == "measure_reads" && /^(rate|rate_iops|exec_prerun)=/ { next }
        section == "measure_writes" && /^(rate|rate_iops|exec_prerun)=/ { next }
        section == "measure_reads" && /^iodepth=/ && read_iodepth != "" {
            print "iodepth=" read_iodepth
            next
        }
        section == "measure_writes" && /^iodepth=/ && write_iodepth != "" {
            print "iodepth=" write_iodepth
            next
        }
        section == "measure_reads" && /^numjobs=/ && read_jobs != "" {
            print "numjobs=" read_jobs
            next
        }
        section == "measure_writes" && /^numjobs=/ && write_jobs != "" {
            print "numjobs=" write_jobs
            next
        }
        section == "measure_reads" && /^io_size=/ && read_io_size != "" {
            print "io_size=" read_io_size
            next
        }
        section == "measure_writes" && /^io_size=/ && write_io_size != "" {
            print "io_size=" write_io_size
            next
        }
        { print }
        END { emit_measure_extras() }
    ' "$FIO_JOB_FILE" > "$fio_job_tmp"

    cp "$fio_job_tmp" "${RESULT_FOLDER%/}/fio_job_${variant}_${ts}.fio" 2>/dev/null || true
    echo "=== fio jobfile: variant=$variant job=$fio_job_tmp output=$out_json ==="
    sudo fio "$fio_job_tmp" --output="$out_json" --output-format=json

    if [[ -w "$FIO_TEST_PHASE_PATH" ]]; then
        echo 0 | sudo tee "$FIO_TEST_PHASE_PATH" >/dev/null || true
    fi

    if [[ -r /sys/kernel/debug/nvmev/ftl0/test_phase_stats ]]; then
        cat /sys/kernel/debug/nvmev/ftl0/test_phase_stats \
            > "${RESULT_FOLDER%/}/fio_test_phase_stats_${variant}_${ts}.txt" || true
    fi
    if [[ -r /sys/kernel/debug/nvmev/ftl0/superblock_stats ]]; then
        cat /sys/kernel/debug/nvmev/ftl0/superblock_stats \
            > "${RESULT_FOLDER%/}/fio_superblock_stats_${variant}_${ts}.txt" || true
    fi

    rm -f "$fio_job_tmp"
    sudo rmmod nvmev 2>/dev/null || rmmod nvmev 2>/dev/null || true
    sleep 5
}

if [[ "$RUN_SQLITE" == "1" ]]; then
    FORCE_REBUILD="${FORCE_REBUILD:-1}" \
    SQLITE_REBUILD_DIE_MODULES="${SQLITE_REBUILD_DIE_MODULES:-1}" \
    THREAD_COUNTS="${THREAD_COUNTS:-1}" \
    VARIANTS="$VARIANTS" \
    SQLITE_ACCESS_DIST_LIST="${SQLITE_ACCESS_DIST_LIST:-zipf}" \
    ZIPF_ALPHA="${ZIPF_ALPHA:-1.2}" \
    NORMAL_MEAN="${NORMAL_MEAN:--1}" \
    NORMAL_STDDEV="${NORMAL_STDDEV:-8}" \
    SQLITE_WINDOW_PAGES_PER_TABLE_LIST="${SQLITE_WINDOW_PAGES_PER_TABLE_LIST:-960}" \
    SQLITE_TABLE_COUNT="${SQLITE_TABLE_COUNT:-80}" \
    SQLITE_ROWS_PER_TABLE="${SQLITE_ROWS_PER_TABLE:-3200}" \
    SQLITE_TARGET_BYTES="${SQLITE_TARGET_BYTES:-8G}" \
    SQLITE_WINDOW_TABLES="${SQLITE_WINDOW_TABLES:-80}" \
    SQLITE_WINDOW_PASSES_PER_ROUND="${SQLITE_WINDOW_PASSES_PER_ROUND:-1}" \
    SQLITE_INTERLEAVE_READS="${SQLITE_INTERLEAVE_READS:-512}" \
    SQLITE_COLD_FULL_READ_MODE="${SQLITE_COLD_FULL_READ_MODE:-random-row-concurrent}" \
    SQLITE_COLD_EXTRA_APPEND_BYTES="${SQLITE_COLD_EXTRA_APPEND_BYTES:-5G}" \
    SQLITE_COLD_EXTRA_MODE="${SQLITE_COLD_EXTRA_MODE:-concurrent}" \
    SQLITE_COLD_EXTRA_READ_RATIO="${SQLITE_COLD_EXTRA_READ_RATIO:-10}" \
    SQLITE_COLD_EXTRA_ROW_READS_PER_BATCH="${SQLITE_COLD_EXTRA_ROW_READS_PER_BATCH:-0}" \
    bash sqlite_fragment_die_test1_tablefile_pageflow_fileparallel_fullscan.sh
fi

if [[ "$RUN_FIO" == "1" ]]; then
    if [[ "${SQLITE_REBUILD_DIE_MODULES:-1}" == "1" ]]; then
        (cd "$NVMEV_DIR" && bash build_die.sh $VARIANTS)
    fi
    for variant in $VARIANTS; do
        run_fio_one_variant "$variant"
    done
fi

echo "=== sqlite/fio variant run completed: $VARIANTS ==="
