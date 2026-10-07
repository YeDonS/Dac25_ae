#!/bin/bash
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
  echo "This script needs root privileges to hit the block device directly." >&2
  exit 1
fi

DEFAULT_DEVICE=${DATA_DEV:-/dev/nvme0n1}
DEVICE=${DEVICE:-$DEFAULT_DEVICE}
RESULT_DIR=${RESULT_DIR:-./result/test_cold}
LOG_FILE="$RESULT_DIR/test_cold_$(date +%Y%m%d_%H%M%S).log"

BASE_OFFSET_MB=${BASE_OFFSET_MB:-0}
WARM_TIER3_SIZE_MB=${WARM_TIER3_SIZE_MB:-1536}   # 广域温区（最低强度）
WARM_TIER2_SIZE_MB=${WARM_TIER2_SIZE_MB:-768}    # 中度温区
WARM_TIER1_SIZE_MB=${WARM_TIER1_SIZE_MB:-384}    # 高度温区
PINNED_HOT_SIZE_MB=${PINNED_HOT_SIZE_MB:-256}    # 常驻热点

: "${WARM_TIER3_OFFSET_MB:=$((BASE_OFFSET_MB))}"
: "${WARM_TIER2_OFFSET_MB:=$((WARM_TIER3_OFFSET_MB + WARM_TIER3_SIZE_MB))}"
: "${WARM_TIER1_OFFSET_MB:=$((WARM_TIER2_OFFSET_MB + WARM_TIER2_SIZE_MB))}"
: "${PINNED_HOT_OFFSET_MB:=$((WARM_TIER1_OFFSET_MB + WARM_TIER1_SIZE_MB))}"

TOTAL_RANGE_MB=$((PINNED_HOT_OFFSET_MB + PINNED_HOT_SIZE_MB - BASE_OFFSET_MB))
WARM_TOTAL_SIZE_MB=$((WARM_TIER3_SIZE_MB + WARM_TIER2_SIZE_MB + WARM_TIER1_SIZE_MB))

if (( TOTAL_RANGE_MB <= 0 || WARM_TOTAL_SIZE_MB <= 0 )); then
  echo "Computed workload window is empty. Check offset/size knobs." >&2
  exit 1
fi

CYCLES=${CYCLES:-7}
PINNED_HOT_RUNTIME=${PINNED_HOT_RUNTIME:-18}
WARM_TIER1_RUNTIME=${WARM_TIER1_RUNTIME:-13}
WARM_TIER2_RUNTIME=${WARM_TIER2_RUNTIME:-9}
WARM_TIER3_RUNTIME=${WARM_TIER3_RUNTIME:-6}
TIER_REFRESH_RUNTIME=${TIER_REFRESH_RUNTIME:-8}
FINAL_SAMPLE_RUNTIME=${FINAL_SAMPLE_RUNTIME:-35}
REST_INTERVAL_SEC=${REST_INTERVAL_SEC:-1}

PINNED_HOT_RWMIXWRITE=${PINNED_HOT_RWMIXWRITE:-68}
PINNED_HOT_BS=${PINNED_HOT_BS:-8k}
PINNED_HOT_NUMJOBS=${PINNED_HOT_NUMJOBS:-4}
PINNED_HOT_ZIPF=${PINNED_HOT_ZIPF:-1.7}

WARM_TIER1_RWMIXWRITE=${WARM_TIER1_RWMIXWRITE:-58}
WARM_TIER1_BS=${WARM_TIER1_BS:-12k}
WARM_TIER1_NUMJOBS=${WARM_TIER1_NUMJOBS:-3}
WARM_TIER1_ZIPF=${WARM_TIER1_ZIPF:-1.4}

WARM_TIER2_RWMIXWRITE=${WARM_TIER2_RWMIXWRITE:-48}
WARM_TIER2_BS=${WARM_TIER2_BS:-20k}
WARM_TIER2_NUMJOBS=${WARM_TIER2_NUMJOBS:-3}
WARM_TIER2_ZIPF=${WARM_TIER2_ZIPF:-1.2}

WARM_TIER3_RWMIXWRITE=${WARM_TIER3_RWMIXWRITE:-40}
WARM_TIER3_BS=${WARM_TIER3_BS:-32k}
WARM_TIER3_NUMJOBS=${WARM_TIER3_NUMJOBS:-2}
WARM_TIER3_ZIPF=${WARM_TIER3_ZIPF:-1.1}

REFRESH_READ_BS=${REFRESH_READ_BS:-48k}
REFRESH_READ_NUMJOBS=${REFRESH_READ_NUMJOBS:-2}
FINAL_SAMPLE_BS=${FINAL_SAMPLE_BS:-24k}
FINAL_SAMPLE_NUMJOBS=${FINAL_SAMPLE_NUMJOBS:-4}

COMMON_FIO_ARGS=(
  --filename="$DEVICE"
  --ioengine=io_uring
  --direct=1
  --group_reporting
  --iodepth=32
  --time_based
  --randrepeat=0
)

if ! command -v fio >/dev/null 2>&1; then
  echo "fio is required but was not found in PATH." >&2
  exit 1
fi

if [[ ! -b $DEVICE ]]; then
  echo "Block device $DEVICE not found. Override DEVICE=/dev/xxx when invoking." >&2
  exit 1
fi

mkdir -p "$RESULT_DIR"

time_stamp() {
  date '+%Y-%m-%d %H:%M:%S'
}

run_phase() {
  local label=$1
  shift
  echo "[$(time_stamp)] >>> $label" | tee -a "$LOG_FILE"
  fio "${COMMON_FIO_ARGS[@]}" "$@" | tee -a "$LOG_FILE"
  echo | tee -a "$LOG_FILE"
  if (( REST_INTERVAL_SEC > 0 )); then
    sleep "$REST_INTERVAL_SEC"
  fi
}

cat <<CONFIG >>"$LOG_FILE"
# test_cold.sh configuration snapshot ($(time_stamp))
DEVICE=$DEVICE
BASE_OFFSET_MB=$BASE_OFFSET_MB
WARM_TIER3_OFFSET_MB=$WARM_TIER3_OFFSET_MB
WARM_TIER3_SIZE_MB=$WARM_TIER3_SIZE_MB
WARM_TIER2_OFFSET_MB=$WARM_TIER2_OFFSET_MB
WARM_TIER2_SIZE_MB=$WARM_TIER2_SIZE_MB
WARM_TIER1_OFFSET_MB=$WARM_TIER1_OFFSET_MB
WARM_TIER1_SIZE_MB=$WARM_TIER1_SIZE_MB
PINNED_HOT_OFFSET_MB=$PINNED_HOT_OFFSET_MB
PINNED_HOT_SIZE_MB=$PINNED_HOT_SIZE_MB
TOTAL_RANGE_MB=$TOTAL_RANGE_MB
CYCLES=$CYCLES
PINNED_HOT_RUNTIME=$PINNED_HOT_RUNTIME
WARM_TIER1_RUNTIME=$WARM_TIER1_RUNTIME
WARM_TIER2_RUNTIME=$WARM_TIER2_RUNTIME
WARM_TIER3_RUNTIME=$WARM_TIER3_RUNTIME
TIER_REFRESH_RUNTIME=$TIER_REFRESH_RUNTIME
FINAL_SAMPLE_RUNTIME=$FINAL_SAMPLE_RUNTIME
PINNED_HOT_RWMIXWRITE=$PINNED_HOT_RWMIXWRITE
PINNED_HOT_BS=$PINNED_HOT_BS
PINNED_HOT_NUMJOBS=$PINNED_HOT_NUMJOBS
PINNED_HOT_ZIPF=$PINNED_HOT_ZIPF
WARM_TIER1_RWMIXWRITE=$WARM_TIER1_RWMIXWRITE
WARM_TIER1_BS=$WARM_TIER1_BS
WARM_TIER1_NUMJOBS=$WARM_TIER1_NUMJOBS
WARM_TIER1_ZIPF=$WARM_TIER1_ZIPF
WARM_TIER2_RWMIXWRITE=$WARM_TIER2_RWMIXWRITE
WARM_TIER2_BS=$WARM_TIER2_BS
WARM_TIER2_NUMJOBS=$WARM_TIER2_NUMJOBS
WARM_TIER2_ZIPF=$WARM_TIER2_ZIPF
WARM_TIER3_RWMIXWRITE=$WARM_TIER3_RWMIXWRITE
WARM_TIER3_BS=$WARM_TIER3_BS
WARM_TIER3_NUMJOBS=$WARM_TIER3_NUMJOBS
WARM_TIER3_ZIPF=$WARM_TIER3_ZIPF
REFRESH_READ_BS=$REFRESH_READ_BS
REFRESH_READ_NUMJOBS=$REFRESH_READ_NUMJOBS
FINAL_SAMPLE_BS=$FINAL_SAMPLE_BS
FINAL_SAMPLE_NUMJOBS=$FINAL_SAMPLE_NUMJOBS
CONFIG

echo "[$(time_stamp)] Using device $DEVICE" | tee -a "$LOG_FILE"
echo "[$(time_stamp)] Logs: $LOG_FILE" | tee -a "$LOG_FILE"

echo 3 > /proc/sys/vm/drop_caches || true

run_phase "Phase 0 - Compact sequential seed" \
  --name=compact_seed \
  --rw=write \
  --bs=256k \
  --offset=${BASE_OFFSET_MB}M \
  --size=${TOTAL_RANGE_MB}M \
  --numjobs=2 \
  --loops=1

run_phase "Phase 1 - Tier bootstrap writes" \
  --name=bootstrap_tiers \
  --rw=randwrite \
  --bs=64k \
  --offset=${WARM_TIER3_OFFSET_MB}M \
  --size=${TOTAL_RANGE_MB}M \
  --numjobs=2 \
  --runtime=12 \
  --random_distribution=zipf:1.3

for ((cycle = 1; cycle <= CYCLES; cycle++)); do
  run_phase "Cycle ${cycle}A - Pinned hot focus" \
    --name=pinned_hot_${cycle} \
    --rw=randrw \
    --rwmixwrite=${PINNED_HOT_RWMIXWRITE} \
    --bs=${PINNED_HOT_BS} \
    --offset=${PINNED_HOT_OFFSET_MB}M \
    --size=${PINNED_HOT_SIZE_MB}M \
    --numjobs=${PINNED_HOT_NUMJOBS} \
    --runtime=${PINNED_HOT_RUNTIME} \
    --random_distribution=zipf:${PINNED_HOT_ZIPF}

  run_phase "Cycle ${cycle}B - Warm tier1 sustain" \
    --name=warm_tier1_${cycle} \
    --rw=randrw \
    --rwmixwrite=${WARM_TIER1_RWMIXWRITE} \
    --bs=${WARM_TIER1_BS} \
    --offset=${WARM_TIER1_OFFSET_MB}M \
    --size=${WARM_TIER1_SIZE_MB}M \
    --numjobs=${WARM_TIER1_NUMJOBS} \
    --runtime=${WARM_TIER1_RUNTIME} \
    --random_distribution=zipf:${WARM_TIER1_ZIPF}

  run_phase "Cycle ${cycle}C - Warm tier2 balance" \
    --name=warm_tier2_${cycle} \
    --rw=randrw \
    --rwmixwrite=${WARM_TIER2_RWMIXWRITE} \
    --bs=${WARM_TIER2_BS} \
    --offset=${WARM_TIER2_OFFSET_MB}M \
    --size=${WARM_TIER2_SIZE_MB}M \
    --numjobs=${WARM_TIER2_NUMJOBS} \
    --runtime=${WARM_TIER2_RUNTIME} \
    --random_distribution=zipf:${WARM_TIER2_ZIPF}

  run_phase "Cycle ${cycle}D - Warm tier3 background" \
    --name=warm_tier3_${cycle} \
    --rw=randrw \
    --rwmixwrite=${WARM_TIER3_RWMIXWRITE} \
    --bs=${WARM_TIER3_BS} \
    --offset=${WARM_TIER3_OFFSET_MB}M \
    --size=${WARM_TIER3_SIZE_MB}M \
    --numjobs=${WARM_TIER3_NUMJOBS} \
    --runtime=${WARM_TIER3_RUNTIME} \
    --random_distribution=zipf:${WARM_TIER3_ZIPF}

  if (( cycle % 2 == 0 )); then
    run_phase "Cycle ${cycle}E - Tier refresh reads" \
      --name=refresh_${cycle} \
      --rw=randread \
      --bs=${REFRESH_READ_BS} \
      --offset=${WARM_TIER3_OFFSET_MB}M \
      --size=${WARM_TOTAL_SIZE_MB}M \
      --numjobs=${REFRESH_READ_NUMJOBS} \
      --runtime=${TIER_REFRESH_RUNTIME}
  fi
done

run_phase "Phase Final - Tier sampling readback" \
  --name=final_sample \
  --rw=randread \
  --bs=${FINAL_SAMPLE_BS} \
  --offset=${WARM_TIER3_OFFSET_MB}M \
  --size=${WARM_TOTAL_SIZE_MB}M \
  --numjobs=${FINAL_SAMPLE_NUMJOBS} \
  --runtime=${FINAL_SAMPLE_RUNTIME} \
  --random_distribution=zipf:1.2

echo "[$(time_stamp)] Test sequence complete." | tee -a "$LOG_FILE"

