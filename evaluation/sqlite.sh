#!/bin/bash -x

source commonvariables.sh

SRC_PATH="sqlite"

gcc -D TARGET_FOLDER=\"$TARGET_FOLDER\" -o ./sqlite_append ./$SRC_PATH/sqlite_append.c -lsqlite3 -lm
gcc -D TARGET_FOLDER=\"$TARGET_FOLDER\" -o ./sqlite_ideal ./$SRC_PATH/sqlite_contiguous.c -lsqlite3

READ_QUERIES=${READ_QUERIES:-6000}
ZIPF_ALPHA=${ZIPF_ALPHA:-1.2}
ZIPF_SEED=${ZIPF_SEED:-42}
EXP_LAMBDA=${EXP_LAMBDA:-0.0008}
EXP_SEED=${EXP_SEED:-4242}
FAST_INIT_ARGS=${FAST_INIT_ARGS:-}

run_fragment_suite() {
    local mode=$1
    local tag=$2

    ./nvmevstart_"$mode".sh
    sleep 1

    lsblk

    source setdevice.sh

    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null

    numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./sqlite_append --mode init \
        $FAST_INIT_ARGS \
        > "./$RESULT_FOLDER/sqlite_${tag}_init.txt"

    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null

    numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./sqlite_append \
        --mode read --distribution zipf --reads "$READ_QUERIES" \
        --alpha "$ZIPF_ALPHA" --seed "$ZIPF_SEED" \
        --log "./$RESULT_FOLDER/sqlite_${tag}_zipf_trace.csv" \
        > "./$RESULT_FOLDER/sqlite_${tag}_zipf.txt"

    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null

    numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./sqlite_append \
        --mode read --distribution exp --reads "$READ_QUERIES" \
        --lambda "$EXP_LAMBDA" --seed "$EXP_SEED" \
        --log "./$RESULT_FOLDER/sqlite_${tag}_exp_trace.csv" \
        > "./$RESULT_FOLDER/sqlite_${tag}_exp.txt"

    source resetdevice.sh
}

./disablemeta.sh

# contiguous
./nvmevstart_off.sh
sleep 1

lsblk

source setdevice.sh

sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null

numactl --cpubind=$NUMADOMAIN --membind=$NUMADOMAIN ./sqlite_ideal >> ./$RESULT_FOLDER/sqlite_contiguous.txt

source resetdevice.sh

run_fragment_suite off append_off
run_fragment_suite on append_on

./enablemeta.sh
