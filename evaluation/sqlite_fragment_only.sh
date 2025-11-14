#!/bin/bash -e

source commonvariables.sh

SRC_PATH="sqlite"

if [[ ! -f ./sqlite_append ]] || [[ $FORCE_REBUILD == 1 ]]; then
    gcc -D TARGET_FOLDER=\"$TARGET_FOLDER\" -o ./sqlite_append ./$SRC_PATH/sqlite_append.c -lsqlite3 -lm
fi

READ_QUERIES=${READ_QUERIES:-6000}
ZIPF_ALPHA=${ZIPF_ALPHA:-1.2}
ZIPF_SEED=${ZIPF_SEED:-42}
EXP_LAMBDA=${EXP_LAMBDA:-0.0008}
EXP_SEED=${EXP_SEED:-4242}
RUN_ZIPF=${RUN_ZIPF:-1}
RUN_EXP=${RUN_EXP:-1}
RUN_NORMAL=${RUN_NORMAL:-0}
NORMAL_MEAN=${NORMAL_MEAN:-5000}
NORMAL_STDDEV=${NORMAL_STDDEV:-800}
NORMAL_SEED=${NORMAL_SEED:-314159}
SQLITE_TARGET_BYTES=${SQLITE_TARGET_BYTES:-6G}
SQLITE_TRACE_DIR=${SQLITE_TRACE_DIR:-}
SCAN_ITERS=${SCAN_ITERS:-10}

ACCESS_COUNT_PATH=${ACCESS_COUNT_PATH:-/sys/kernel/debug/nvmev/ftl0/access_count}
ACCESS_INJECT_PATH=${ACCESS_INJECT_PATH:-/sys/kernel/debug/nvmev/ftl0/access_inject}

ensure_access_inject() {
    local debugfs_mount
    debugfs_mount=$(mount | grep -w debugfs || true)
    if [[ -z $debugfs_mount ]]; then
        sudo mount -t debugfs debugfs /sys/kernel/debug
    fi

    local inject_path="$ACCESS_INJECT_PATH"
    local count_path="$ACCESS_COUNT_PATH"
    local attempts=40
    while (( attempts > 0 )); do
        if [[ -w $inject_path && -r $count_path ]]; then
            return 0
        fi
        sleep 0.25
        attempts=$((attempts - 1))
    done

    echo "access inject/count nodes not ready under /sys/kernel/debug" >&2
    exit 1
}

run_fragment_suite() {
    local mode=$1
    local tag=$2

    ./nvmevstart_"$mode".sh
    sleep 1
    lsblk
    source setdevice.sh
    ensure_access_inject

    local trace_dir="${SQLITE_TRACE_DIR:-${TARGET_FOLDER%/}/sqlite_traces}"
    mkdir -p "$trace_dir"
    local trace_base="${trace_dir%/}"

    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
    numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./sqlite_append --mode init \
        --target-bytes "$SQLITE_TARGET_BYTES" \
        --zipf-seed "$ZIPF_SEED" \
        --exp-seed "$EXP_SEED" \
        --normal-seed "$NORMAL_SEED" \
        --alpha "$ZIPF_ALPHA" \
        --lambda "$EXP_LAMBDA" \
        --normal-mean "$NORMAL_MEAN" \
        --normal-stddev "$NORMAL_STDDEV" \
        --tag "$tag" \
        --trace-dir "$trace_dir" \
        > "./$RESULT_FOLDER/sqlite_${tag}_init.txt"

    if [[ $RUN_ZIPF -eq 1 ]]; then
        local trace_zipf="${trace_base}/sqlite_trace_${tag}_zipf.trace"
        if [[ ! -f "$trace_zipf" ]]; then
            echo "Trace file not found: $trace_zipf" >&2
            exit 1
        fi

        sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
        numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./sqlite_append \
            --mode read --distribution zipf --reads "$READ_QUERIES" \
            --alpha "$ZIPF_ALPHA" --seed "$ZIPF_SEED" \
            --tag "$tag" \
            --trace-mode replay --trace-path "$trace_zipf" \
            --suppress-report \
            > "./$RESULT_FOLDER/sqlite_${tag}_zipf_warm.txt"

        sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
        numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./sqlite_append \
            --mode read --distribution zipf --reads "$READ_QUERIES" \
            --alpha "$ZIPF_ALPHA" --seed "$ZIPF_SEED" \
            --tag "$tag" \
            --trace-mode replay --trace-path "$trace_zipf" \
            --log "./$RESULT_FOLDER/sqlite_${tag}_zipf_trace.csv" \
            --heatmap "./$RESULT_FOLDER/sqlite_${tag}_zipf_heat.csv" \
            --human-log \
            > "./$RESULT_FOLDER/sqlite_${tag}_zipf.txt"
    fi

    if [[ $RUN_EXP -eq 1 ]]; then
        local trace_exp="${trace_base}/sqlite_trace_${tag}_exp.trace"
        if [[ ! -f "$trace_exp" ]]; then
            echo "Trace file not found: $trace_exp" >&2
            exit 1
        fi

        sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
        numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./sqlite_append \
            --mode read --distribution exp --reads "$READ_QUERIES" \
            --lambda "$EXP_LAMBDA" --seed "$EXP_SEED" \
            --tag "$tag" \
            --trace-mode replay --trace-path "$trace_exp" \
            --suppress-report \
            > "./$RESULT_FOLDER/sqlite_${tag}_exp_warm.txt"

        sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
        numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./sqlite_append \
            --mode read --distribution exp --reads "$READ_QUERIES" \
            --lambda "$EXP_LAMBDA" --seed "$EXP_SEED" \
            --tag "$tag" \
            --trace-mode replay --trace-path "$trace_exp" \
            --log "./$RESULT_FOLDER/sqlite_${tag}_exp_trace.csv" \
            --heatmap "./$RESULT_FOLDER/sqlite_${tag}_exp_heat.csv" \
            --human-log \
            > "./$RESULT_FOLDER/sqlite_${tag}_exp.txt"
    fi

    if [[ $RUN_NORMAL -eq 1 ]]; then
        local trace_normal="${trace_base}/sqlite_trace_${tag}_normal.trace"
        if [[ ! -f "$trace_normal" ]]; then
            echo "Trace file not found: $trace_normal" >&2
            exit 1
        fi

        sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
        numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./sqlite_append \
            --mode read --distribution normal --reads "$READ_QUERIES" \
            --normal-mean "$NORMAL_MEAN" --normal-stddev "$NORMAL_STDDEV" \
            --seed "$NORMAL_SEED" \
            --tag "$tag" \
            --trace-mode replay --trace-path "$trace_normal" \
            --suppress-report \
            > "./$RESULT_FOLDER/sqlite_${tag}_normal_warm.txt"

        sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
        numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./sqlite_append \
            --mode read --distribution normal --reads "$READ_QUERIES" \
            --normal-mean "$NORMAL_MEAN" --normal-stddev "$NORMAL_STDDEV" \
            --seed "$NORMAL_SEED" \
            --tag "$tag" \
            --trace-mode replay --trace-path "$trace_normal" \
            --log "./$RESULT_FOLDER/sqlite_${tag}_normal_trace.csv" \
            --heatmap "./$RESULT_FOLDER/sqlite_${tag}_normal_heat.csv" \
            --human-log \
            > "./$RESULT_FOLDER/sqlite_${tag}_normal.txt"
    fi

    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
    numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./sqlite_append \
        --mode scan --scan-iters "$SCAN_ITERS" \
        --tag "$tag" \
        > "./$RESULT_FOLDER/sqlite_${tag}_scan.txt"

    source resetdevice.sh
}

target_modes=("off" "on")
if [[ $# -gt 0 ]]; then
    target_modes=("$@")
fi

./disablemeta.sh

for mode in "${target_modes[@]}"; do
    run_fragment_suite "$mode" "fragment_${mode}"
done

./enablemeta.sh
