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

run_fragment_suite() {
    local mode=$1
    local tag=$2

    ./nvmevstart_"$mode".sh
    sleep 1
    lsblk
    source setdevice.sh

    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
    numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./sqlite_append --mode init \
        > "./$RESULT_FOLDER/sqlite_${tag}_init.txt"

    if [[ $RUN_ZIPF -eq 1 ]]; then
        sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
        numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./sqlite_append \
            --mode read --distribution zipf --reads "$READ_QUERIES" \
            --alpha "$ZIPF_ALPHA" --seed "$ZIPF_SEED" \
            --log "./$RESULT_FOLDER/sqlite_${tag}_zipf_trace.csv" \
            --heatmap "./$RESULT_FOLDER/sqlite_${tag}_zipf_heat.csv" \
            --human-log \
            > "./$RESULT_FOLDER/sqlite_${tag}_zipf.txt"
    fi

    if [[ $RUN_EXP -eq 1 ]]; then
        sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
        numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./sqlite_append \
            --mode read --distribution exp --reads "$READ_QUERIES" \
            --lambda "$EXP_LAMBDA" --seed "$EXP_SEED" \
            --log "./$RESULT_FOLDER/sqlite_${tag}_exp_trace.csv" \
            --heatmap "./$RESULT_FOLDER/sqlite_${tag}_exp_heat.csv" \
            --human-log \
            > "./$RESULT_FOLDER/sqlite_${tag}_exp.txt"
    fi

    if [[ $RUN_NORMAL -eq 1 ]]; then
        sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
        numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./sqlite_append \
            --mode read --distribution normal --reads "$READ_QUERIES" \
            --normal-mean "$NORMAL_MEAN" --normal-stddev "$NORMAL_STDDEV" \
            --seed "$NORMAL_SEED" \
            --log "./$RESULT_FOLDER/sqlite_${tag}_normal_trace.csv" \
            --heatmap "./$RESULT_FOLDER/sqlite_${tag}_normal_heat.csv" \
            --human-log \
            > "./$RESULT_FOLDER/sqlite_${tag}_normal.txt"
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
