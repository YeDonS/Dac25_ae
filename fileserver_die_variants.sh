#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

source commonvariables.sh

APPEND_PROFILE="${APPEND_PROFILE:-small}"
VARIANTS="${VARIANTS:-die_base die_no1 die_no2 die_no3 die_all}"
READ_RUNS="${READ_RUNS:-1}"

NVMEV_DIR="${SCRIPT_DIR}/../nvmevirt_DA"

case "$APPEND_PROFILE" in
  small)
    APPEND_F="filebench/fileserver_append_small.f"
    RESULT_PREFIX="fileserver_small"
    ;;
  normal)
    APPEND_F="filebench/fileserver_append.f"
    RESULT_PREFIX="fileserver"
    ;;
  *)
    echo "ERROR: APPEND_PROFILE must be 'small' or 'normal'" >&2
    exit 1
    ;;
esac

READ_F="filebench/fileserver_read.f"
DIE_RESULT_BASE="${RESULT_FOLDER%/}/${RESULT_PREFIX}_die_variants"

drop_caches() {
    sync
    echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
}

load_die_module() {
    local variant="$1"
    local ko_path="${NVMEV_DIR}/nvmev_${variant}.ko"

    if [[ ! -f "$ko_path" ]]; then
        echo "ERROR: $ko_path not found. Run build_die.sh first." >&2
        exit 1
    fi

    echo "=== Loading $ko_path (via nvmevstart_on.sh) ==="
    if [[ -f ./nvmev_on.ko ]]; then
        cp ./nvmev_on.ko ./nvmev_on.ko.die_bak
    fi
    cp "$ko_path" ./nvmev_on.ko
    ./nvmevstart_on.sh
    if [[ -f ./nvmev_on.ko.die_bak ]]; then
        mv ./nvmev_on.ko.die_bak ./nvmev_on.ko
    fi
    sleep 1
}

capture_tree_size() {
    local root="$1"
    local out="$2"

    {
        echo "[tree_size]"
        if [[ -d "$root" ]]; then
            du -sh "$root" 2>/dev/null || true
            du -sb "$root" 2>/dev/null || true
            find "$root" -type f 2>/dev/null | wc -l | awk '{print "files=" $1}'
        else
            echo "missing=$root"
        fi
    } >"$out"
}

run_one_variant() {
    local variant="$1"
    local out_dir="${DIE_RESULT_BASE}/${variant}"
    local append_log="${RESULT_FOLDER%/}/${RESULT_PREFIX}_append_${variant}.txt"
    local read_log="${RESULT_FOLDER%/}/${RESULT_PREFIX}_read_${variant}.txt"
    local size_log="${RESULT_FOLDER%/}/${RESULT_PREFIX}_size_${variant}.txt"

    mkdir -p "$out_dir"

    echo ""
    echo "================================================================"
    echo "  [FILESERVER-DIE] variant=$variant profile=$APPEND_PROFILE"
    echo "  append=$APPEND_F read=$READ_F"
    echo "================================================================"

    drop_caches
    sleep 2

    load_die_module "$variant"

    lsblk
    source setdevice.sh
    sleep 1

    echo "=== APPEND: $variant ==="
    filebench -f "$APPEND_F" | tee "$append_log"

    capture_tree_size "${TARGET_FOLDER%/}/bigfileset" "$size_log"
    cat "$size_log"

    sleep 5

    : >"$read_log"
    for reads in $(seq 1 "$READ_RUNS"); do
        echo "=== READ ${reads}/${READ_RUNS}: $variant ===" | tee -a "$read_log"
        drop_caches
        sleep 2
        numactl --cpubind="$NUMADOMAIN" --membind="$NUMADOMAIN" \
            filebench -f "$READ_F" | tee -a "$read_log"
    done

    if [[ -r /sys/kernel/debug/nvmev/ftl0/die_affinity_stats ]]; then
        cp /sys/kernel/debug/nvmev/ftl0/die_affinity_stats \
           "${RESULT_FOLDER%/}/${RESULT_PREFIX}_die_affinity_${variant}.txt" 2>/dev/null || true
    fi
    if [[ -r /sys/kernel/debug/nvmev/ftl0/lpn_die_change_stats ]]; then
        cp /sys/kernel/debug/nvmev/ftl0/lpn_die_change_stats \
           "${RESULT_FOLDER%/}/${RESULT_PREFIX}_lpn_die_change_${variant}.txt" 2>/dev/null || true
    fi

    cp "$append_log" "$out_dir/" 2>/dev/null || true
    cp "$read_log" "$out_dir/" 2>/dev/null || true
    cp "$size_log" "$out_dir/" 2>/dev/null || true
    cp "${RESULT_FOLDER%/}/${RESULT_PREFIX}_die_affinity_${variant}.txt" "$out_dir/" 2>/dev/null || true
    cp "${RESULT_FOLDER%/}/${RESULT_PREFIX}_lpn_die_change_${variant}.txt" "$out_dir/" 2>/dev/null || true

    source resetdevice.sh
    sleep 1
}

echo 0 > /proc/sys/kernel/randomize_va_space
./disablemeta.sh
mkdir -p "$DIE_RESULT_BASE"

for variant in $VARIANTS; do
    run_one_variant "$variant"
done

./enablemeta.sh

echo ""
echo "========================================"
echo "  [FILESERVER-DIE] All tests completed."
echo "  Results in: $DIE_RESULT_BASE"
echo "========================================"
