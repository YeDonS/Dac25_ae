#!/bin/bash -e

source commonvariables.sh

SRC_PATH="mldb"

if [[ ! -f ./mldb_workload ]] || [[ $FORCE_REBUILD == 1 ]]; then
    gcc -D TARGET_FOLDER=\"$TARGET_FOLDER\" -o ./mldb_workload ./$SRC_PATH/mldb_workload.c -lm
fi

READ_QUERIES=${READ_QUERIES:-6000}
ZIPF_ALPHA=${ZIPF_ALPHA:-1.2}
ZIPF_SEED=${ZIPF_SEED:-42}
ZIPF_REREAD_SEED=${ZIPF_REREAD_SEED:-$ZIPF_SEED}
EXP_LAMBDA=${EXP_LAMBDA:-0.0008}
EXP_SEED=${EXP_SEED:-4242}
EXP_REREAD_SEED=${EXP_REREAD_SEED:-$EXP_SEED}
RUN_ZIPF=${RUN_ZIPF:-1}
RUN_EXP=${RUN_EXP:-1}
RUN_NORMAL=${RUN_NORMAL:-0}
NORMAL_MEAN=${NORMAL_MEAN:-5000}
NORMAL_STDDEV=${NORMAL_STDDEV:-800}
NORMAL_SEED=${NORMAL_SEED:-314159}
NORMAL_REREAD_SEED=${NORMAL_REREAD_SEED:-$NORMAL_SEED}

run_read_passes() {
    local dist=$1
    local tag=$2
    local first_seed=$3
    local second_seed=$4
    shift 4
    local extra_args=("$@")
    local passes=("read1" "read2")
    local seeds=("$first_seed" "$second_seed")

    for idx in "${!passes[@]}"; do
        local pass="${passes[$idx]}"
        local seed="${seeds[$idx]}"

        sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
        numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./mldb_workload \
            --layout fragment --mode read --distribution "$dist" \
            --reads "$READ_QUERIES" --seed "$seed" \
            "${extra_args[@]}" \
            --log "./$RESULT_FOLDER/mldb_${tag}_${dist}_${pass}_trace.csv" \
            --heatmap "./$RESULT_FOLDER/mldb_${tag}_${dist}_${pass}_heat.csv" \
            --human-log \
            > "./$RESULT_FOLDER/mldb_${tag}_${dist}_${pass}.txt"
    done
}

run_fragment_suite() {
    local mode=$1
    local tag=$2

    ./nvmevstart_"$mode".sh
    sleep 1
    lsblk
    source setdevice.sh

    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
    numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./mldb_workload \
        --layout fragment --mode init \
        > "./$RESULT_FOLDER/mldb_${tag}_init.txt"

    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
    numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./mldb_workload \
        --layout fragment --mode append \
        > "./$RESULT_FOLDER/mldb_${tag}_append.txt"

    if [[ $RUN_ZIPF -eq 1 ]]; then
        run_read_passes "zipf" "$tag" "$ZIPF_SEED" "$ZIPF_REREAD_SEED" \
            --alpha "$ZIPF_ALPHA"
    fi

    if [[ $RUN_EXP -eq 1 ]]; then
        run_read_passes "exp" "$tag" "$EXP_SEED" "$EXP_REREAD_SEED" \
            --lambda "$EXP_LAMBDA"
    fi

    if [[ $RUN_NORMAL -eq 1 ]]; then
        run_read_passes "normal" "$tag" "$NORMAL_SEED" "$NORMAL_REREAD_SEED" \
            --normal-mean "$NORMAL_MEAN" --normal-stddev "$NORMAL_STDDEV"
    fi

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
