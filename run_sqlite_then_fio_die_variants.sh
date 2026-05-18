#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

source commonvariables.sh

NVMEV_DIR="${SCRIPT_DIR}/../nvmevirt_DA"
VARIANTS="${VARIANTS:-die_base_sb die_latency_sb_v1 die_latency_sb die_all_sb}"
RUN_SQLITE="${RUN_SQLITE:-1}"
RUN_FIO="${RUN_FIO:-1}"
FIO_INIT_BYTES="${FIO_INIT_BYTES:-8G}"
FIO_PREWARM_READ_BYTES="${FIO_PREWARM_READ_BYTES:-32G}"
FIO_MEASURE_READ_BYTES="${FIO_MEASURE_READ_BYTES:-40G}"
FIO_MEASURE_WRITE_BYTES="${FIO_MEASURE_WRITE_BYTES:-4G}"
FIO_READ_JOBS="${FIO_READ_JOBS:-2}"
FIO_WRITE_JOBS="${FIO_WRITE_JOBS:-2}"
FIO_READ_IODEPTH="${FIO_READ_IODEPTH:-16}"
FIO_WRITE_IODEPTH="${FIO_WRITE_IODEPTH:-32}"
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

    ts="$(date +%Y%m%d_%H%M%S)"
    out_json="${RESULT_FOLDER%/}/fio_latency_mixed_${variant}_${ts}.json"

    load_variant_module "$variant"
    lsblk
    source setdevice.sh
    echo 0 | sudo tee /sys/block/${DATA_NAME}/queue/read_ahead_kb >/dev/null 2>&1 || true
    drop_caches

    if [[ -w "$FIO_TEST_PHASE_PATH" ]]; then
        echo 0 | sudo tee "$FIO_TEST_PHASE_PATH" >/dev/null || true
    fi

    echo "=== fio init/prewarm: variant=$variant ==="
    sudo fio \
        --name=init_write \
        --filename="$DATA_DEV" \
        --ioengine=libaio \
        --direct=1 \
        --rw=write \
        --bs=4k \
        --iodepth=32 \
        --size="$FIO_INIT_BYTES" \
        --numjobs=1 \
        --group_reporting=1

    sudo fio \
        --name=prewarm_normal_read \
        --filename="$DATA_DEV" \
        --ioengine=libaio \
        --direct=1 \
        --rw=randread \
        --random_distribution=normal \
        --bs=4k \
        --iodepth=32 \
        --size="$FIO_INIT_BYTES" \
        --io_size="$FIO_PREWARM_READ_BYTES" \
        --numjobs=2 \
        --group_reporting=1

    if [[ -w "$FIO_TEST_PHASE_PATH" ]]; then
        echo 1 | sudo tee "$FIO_TEST_PHASE_PATH" >/dev/null || true
    fi

    echo "=== fio measured read/write: variant=$variant output=$out_json ==="
    sudo fio \
        --ioengine=libaio \
        --direct=1 \
        --thread=1 \
        --group_reporting=1 \
        --time_based=0 \
        --norandommap=1 \
        --randrepeat=0 \
        --invalidate=1 \
        --bs=4k \
        --filename="$DATA_DEV" \
        --name=measure_reads \
        --rw=randread \
        --random_distribution=normal \
        --offset=0 \
        --size="$FIO_INIT_BYTES" \
        --iodepth="$FIO_READ_IODEPTH" \
        --numjobs="$FIO_READ_JOBS" \
        --io_size="$FIO_MEASURE_READ_BYTES" \
        --write_lat_log="fio_${variant}_read" \
        --log_avg_msec=1000 \
        --name=measure_writes \
        --rw=randwrite \
        --random_distribution=normal \
        --offset=0 \
        --size="$FIO_INIT_BYTES" \
        --iodepth="$FIO_WRITE_IODEPTH" \
        --numjobs="$FIO_WRITE_JOBS" \
        --io_size="$FIO_MEASURE_WRITE_BYTES" \
        --write_lat_log="fio_${variant}_write" \
        --log_avg_msec=1000 \
        --output="$out_json" \
        --output-format=json

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

    source resetdevice.sh
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
