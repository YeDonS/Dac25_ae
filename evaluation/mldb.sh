#!/bin/bash -x

source commonvariables.sh

SRC_PATH="mldb"

gcc -D TARGET_FOLDER=\"$TARGET_FOLDER\" -o ./mldb_workload ./$SRC_PATH/mldb_workload.c -lm

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
FRAGMENT_READ_PASSES=${FRAGMENT_READ_PASSES:-2}
CONTIG_READ_PASSES=${CONTIG_READ_PASSES:-1}

run_read_passes() {
    local layout=$1
    local dist=$2
    local tag=$3
    local pass_count=$4
    local first_seed=$5
    local second_seed=$6
    shift 6
    local extra_args=("$@")
    local passes=("read1" "read2")
    local seeds=("$first_seed" "$second_seed")

    if [[ $pass_count -lt 1 ]]; then
        return
    fi

    for ((idx=0; idx<pass_count && idx<${#passes[@]}; idx++)); do
        local pass="${passes[$idx]}"
        local seed="${seeds[$idx]}"

        sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
        numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./mldb_workload \
            --layout "$layout" --mode read --distribution "$dist" \
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
        run_read_passes "fragment" "zipf" "$tag" "$FRAGMENT_READ_PASSES" \
            "$ZIPF_SEED" "$ZIPF_REREAD_SEED" \
            --alpha "$ZIPF_ALPHA"
    fi

    if [[ $RUN_EXP -eq 1 ]]; then
        run_read_passes "fragment" "exp" "$tag" "$FRAGMENT_READ_PASSES" \
            "$EXP_SEED" "$EXP_REREAD_SEED" \
            --lambda "$EXP_LAMBDA"
    fi

    if [[ $RUN_NORMAL -eq 1 ]]; then
        run_read_passes "fragment" "normal" "$tag" "$FRAGMENT_READ_PASSES" \
            "$NORMAL_SEED" "$NORMAL_REREAD_SEED" \
            --normal-mean "$NORMAL_MEAN" --normal-stddev "$NORMAL_STDDEV"
    fi

    source resetdevice.sh
}

run_contiguous_suite() {
    ./nvmevstart_off.sh
    sleep 1
    lsblk
    source setdevice.sh

    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
    numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./mldb_workload \
        --layout contiguous --mode init \
        > "./$RESULT_FOLDER/mldb_contiguous_init.txt"

    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
    numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./mldb_workload \
        --layout contiguous --mode append \
        > "./$RESULT_FOLDER/mldb_contiguous_append.txt"

    if [[ $RUN_ZIPF -eq 1 ]]; then
        run_read_passes "contiguous" "zipf" "contiguous" "$CONTIG_READ_PASSES" \
            "$ZIPF_SEED" "$ZIPF_REREAD_SEED" \
            --alpha "$ZIPF_ALPHA"
    fi

    if [[ $RUN_EXP -eq 1 ]]; then
        run_read_passes "contiguous" "exp" "contiguous" "$CONTIG_READ_PASSES" \
            "$EXP_SEED" "$EXP_REREAD_SEED" \
            --lambda "$EXP_LAMBDA"
    fi

    if [[ $RUN_NORMAL -eq 1 ]]; then
        run_read_passes "contiguous" "normal" "contiguous" "$CONTIG_READ_PASSES" \
            "$NORMAL_SEED" "$NORMAL_REREAD_SEED" \
            --normal-mean "$NORMAL_MEAN" --normal-stddev "$NORMAL_STDDEV"
    fi

    source resetdevice.sh
}

./disablemeta.sh

run_contiguous_suite
run_fragment_suite off fragment_off
run_fragment_suite on fragment_on

./enablemeta.sh
